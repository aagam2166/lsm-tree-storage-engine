#include "db/db_impl.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace lsm {

Status DB::Open(const std::string& path, const Options& options,
                std::unique_ptr<DB>* db_out) {
  auto impl = std::make_unique<DBImpl>(path, options);
  Status s = impl->Initialize();
  if (!s.ok()) return s;
  *db_out = std::move(impl);
  return Status::OK();
}

DBImpl::DBImpl(const std::string& path, const Options& options)
    : db_path_(path),
      options_(options),
      versions_(path, options),
      range_registry_(),
      compaction_metrics_(),
      active_mem_(nullptr),
      active_wal_(nullptr),
      next_wal_segment_id_(1),
      immutable_count_(0),
      stop_flush_(false),
      closed_(false),
      fg_bytes_written_(0),
      total_lookups_(0),
      sstable_files_touched_(0),
      write_stall_count_(0) {}

DBImpl::~DBImpl() { Close(); }

Status DBImpl::Initialize() {
  ::mkdir(db_path_.c_str(), 0755);

  Status s = RecoverFromDisk();
  if (!s.ok()) return s;

  uint64_t seg_id =
      next_wal_segment_id_.fetch_add(1, std::memory_order_relaxed);
  auto wal = std::make_shared<WalWriter>(versions_.WalFilePath(seg_id), seg_id,
                                         options_.sync_policy,
                                         options_.group_commit_interval_us);
  s = wal->Open();
  if (!s.ok()) return s;

  MemTable* mem =
      MemTable::Create(options_.memtable_type, options_.sharded_memtable_shards);
  mem->SetWalSegmentId(seg_id);

  current_wal_holder_ = wal;
  // Release stores publish initial WAL and MemTable before worker threads start.
  active_wal_.store(wal.get(), std::memory_order_release);
  active_mem_.store(mem, std::memory_order_release);

  compaction_pool_ = std::make_unique<CompactionPool>(
      options_.compaction_threads, &versions_, options_, &range_registry_,
      &compaction_metrics_);
  flush_thread_ = std::thread(&DBImpl::FlushWorkerLoop, this);
  return Status::OK();
}

Status DBImpl::Put(std::string_view key, std::string_view value) {
  return WriteImpl(ValueType::kPut, key, value);
}

Status DBImpl::Delete(std::string_view key) {
  return WriteImpl(ValueType::kDelete, key, "");
}

Status DBImpl::WriteImpl(ValueType type, std::string_view key,
                         std::string_view value) {
  // Backpressure check (Section 3.3): if immutable queue reaches max cap, yield
  // briefly and increment write_stall_count_ metric.
  if (immutable_count_.load(std::memory_order_acquire) >=
      options_.max_immutable_memtables) {
    // Relaxed fetch_add for write stall telemetry.
    write_stall_count_.fetch_add(1, std::memory_order_relaxed);
    while (immutable_count_.load(std::memory_order_acquire) >=
           options_.max_immutable_memtables) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

  // Acquire sequence number atomically (monotonically increasing via fetch_add)
  const uint64_t seq = versions_.FetchAddSequence();

  // Pin active MemTable for this in-flight write (Section 3.3)
  MemTable* mem = nullptr;
  WalWriter* wal = nullptr;
  while (true) {
    mem = active_mem_.load(std::memory_order_acquire);
    if (mem == nullptr) {
      std::this_thread::yield();
      continue;
    }
    mem->BeginWrite();
    wal = active_wal_.load(std::memory_order_acquire);
    if (active_mem_.load(std::memory_order_acquire) == mem &&
        wal != nullptr && mem->GetWalSegmentId() == wal->segment_id()) {
      break;
    }
    mem->EndWrite();
  }
  Status s = wal->AppendRecord(seq, type, key, value);
  if (!s.ok()) {
    mem->EndWrite();
    return s;
  }

  // Insert into Active MemTable
  mem->Insert(seq, type, key, value);
  std::size_t approx_size = mem->ApproximateSize();
  mem->EndWrite();

  // Relaxed fetch_add tracks foreground bytes written.
  fg_bytes_written_.fetch_add(kWalHeaderSize + value.size(),
                              std::memory_order_relaxed);

  // If MemTable reached size limit, atomically swap it (Section 3.3)
  if (approx_size >= options_.memtable_max_size) {
    TryRotateMemTable(mem);
  }

  return Status::OK();
}

void DBImpl::TryRotateMemTable(MemTable* full_mem) {
  // Exactly one writer thread wins TryClaimSwap() on `full_mem`; all other
  // writers return immediately without waiting (Section 3.3).
  if (!full_mem->TryClaimSwap()) {
    return;
  }

  uint64_t new_seg_id =
      next_wal_segment_id_.fetch_add(1, std::memory_order_relaxed);
  auto new_wal = std::make_shared<WalWriter>(
      versions_.WalFilePath(new_seg_id), new_seg_id, options_.sync_policy,
      options_.group_commit_interval_us);
  if (!new_wal->Open().ok()) {
    return;
  }

  MemTable* new_mem =
      MemTable::Create(options_.memtable_type, options_.sharded_memtable_shards);
  new_mem->SetWalSegmentId(new_seg_id);

  {
    std::lock_guard<std::mutex> lock(flush_mu_);
    MemTable* expected = full_mem;
    if (!active_mem_.compare_exchange_strong(
            expected, new_mem, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      new_mem->Unref();
      return;
    }

    active_wal_.store(new_wal.get(), std::memory_order_release);
    retired_wals_.push_back(std::move(current_wal_holder_));
    current_wal_holder_ = std::move(new_wal);
    immutable_list_.push_back(full_mem);
    immutable_count_.fetch_add(1, std::memory_order_release);
  }
  flush_cv_.notify_one();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  // Relaxed fetch_add for total lookups counter.
  total_lookups_.fetch_add(1, std::memory_order_relaxed);

  char padded_key[kDefaultKeySize];
  std::memset(padded_key, 0, kDefaultKeySize);
  std::memcpy(padded_key, key.data(),
              key.size() < kDefaultKeySize ? key.size() : kDefaultKeySize);
  std::string_view ukey(padded_key, kDefaultKeySize);

  // Take references on Active MemTable, Immutable MemTables, and current Version
  // Read path order (Section 2 & 3.8):
  // Active MemTable > Immutable MemTable (newest first) > L0 (newest first) > L1..Ln.
  MemTable* active = nullptr;
  std::vector<MemTable*> imm_snapshot;
  {
    std::lock_guard<std::mutex> lock(flush_mu_);
    // Acquire load of active_mem_.
    active = active_mem_.load(std::memory_order_acquire);
    if (active != nullptr) active->Ref();
    imm_snapshot = immutable_list_;
    for (MemTable* m : imm_snapshot) {
      m->Ref();
    }
  }

  std::string found_val;
  ValueType found_type = ValueType::kPut;
  uint64_t found_seq = 0;

  // 1. Search Active MemTable
  if (active != nullptr) {
    if (active->Get(ukey, &found_val, &found_type, &found_seq)) {
      active->Unref();
      for (MemTable* m : imm_snapshot) m->Unref();
      if (found_type == ValueType::kDelete) return Status::NotFound();
      if (value != nullptr) *value = std::move(found_val);
      return Status::OK();
    }
    active->Unref();
  }

  // 2. Search Immutable MemTables (newest first = back to front)
  for (auto it = imm_snapshot.rbegin(); it != imm_snapshot.rend(); ++it) {
    if ((*it)->Get(ukey, &found_val, &found_type, &found_seq)) {
      for (MemTable* m : imm_snapshot) m->Unref();
      if (found_type == ValueType::kDelete) return Status::NotFound();
      if (value != nullptr) *value = std::move(found_val);
      return Status::OK();
    }
  }
  for (MemTable* m : imm_snapshot) m->Unref();

  // 3. Search On-Disk Levels via immutable Version snapshot
  Version* ver = versions_.AcquireCurrentVersion();
  bool hit = false;
  uint64_t touched = 0;
  Status s = ver->Get(ukey, &hit, &found_val, &found_type, &found_seq, &touched);
  ver->Unref();

  if (touched > 0) {
    // Relaxed fetch_add accumulates SSTables touched per lookup (Read Amplification).
    sstable_files_touched_.fetch_add(touched, std::memory_order_relaxed);
  }
  if (!s.ok()) return s;
  if (!hit || found_type == ValueType::kDelete) {
    return Status::NotFound();
  }
  if (value != nullptr) *value = std::move(found_val);
  return Status::OK();
}

Status DBImpl::Scan(
    std::string_view start_key, std::size_t limit,
    std::vector<std::pair<std::string, std::string>>* results) {
  if (results == nullptr) {
    return Status::InvalidArgument("null results vector");
  }
  results->clear();
  if (limit == 0) return Status::OK();

  char padded_key[kDefaultKeySize] = {};
  std::memcpy(padded_key, start_key.data(),
              std::min(start_key.size(), kDefaultKeySize));
  InternalKey target(std::string_view(padded_key, kDefaultKeySize), UINT64_MAX,
                     ValueType::kPut);

  MemTable* active = nullptr;
  std::vector<MemTable*> imm_snapshot;
  {
    std::lock_guard<std::mutex> lock(flush_mu_);
    active = active_mem_.load(std::memory_order_acquire);
    if (active != nullptr) active->Ref();
    imm_snapshot = immutable_list_;
    for (MemTable* m : imm_snapshot) {
      m->Ref();
    }
  }

  std::vector<std::unique_ptr<InternalIterator>> iters;
  if (active != nullptr) {
    auto it = active->NewIterator();
    it->Seek(target);
    iters.push_back(std::move(it));
  }
  for (MemTable* m : imm_snapshot) {
    auto it = m->NewIterator();
    it->Seek(target);
    iters.push_back(std::move(it));
  }

  Version* ver = versions_.AcquireCurrentVersion();
  for (std::size_t lvl = 0; lvl < ver->NumLevels(); ++lvl) {
    for (const auto& f : ver->LevelFiles(lvl)) {
      if (f.reader) {
        auto it = f.reader->NewIterator();
        it->Seek(target);
        iters.push_back(std::move(it));
      }
    }
  }

  char last_user_key[kDefaultKeySize] = {};
  bool have_last_key = false;

  while (results->size() < limit) {
    int smallest_idx = -1;
    for (std::size_t i = 0; i < iters.size(); ++i) {
      if (!iters[i]->Valid()) continue;
      if (smallest_idx < 0 ||
          iters[i]->key().Compare(
              iters[static_cast<std::size_t>(smallest_idx)]->key()) < 0) {
        smallest_idx = static_cast<int>(i);
      }
    }
    if (smallest_idx < 0) break;

    InternalKey ikey = iters[static_cast<std::size_t>(smallest_idx)]->key();
    std::string val(iters[static_cast<std::size_t>(smallest_idx)]->value());
    iters[static_cast<std::size_t>(smallest_idx)]->Next();

    bool is_first_for_user_key =
        !have_last_key ||
        std::memcmp(ikey.user_key, last_user_key, kDefaultKeySize) != 0;

    if (is_first_for_user_key) {
      std::memcpy(last_user_key, ikey.user_key, kDefaultKeySize);
      have_last_key = true;
      if (ikey.type == ValueType::kPut) {
        results->emplace_back(ikey.UserKeyString(), std::move(val));
      }
    }
  }

  ver->Unref();
  if (active != nullptr) active->Unref();
  for (MemTable* m : imm_snapshot) m->Unref();

  return Status::OK();
}

DBStats DBImpl::GetStats() const {
  DBStats st;
  st.foreground_bytes_written =
      fg_bytes_written_.load(std::memory_order_relaxed);
  st.compaction_bytes_read =
      compaction_metrics_.bytes_read.load(std::memory_order_relaxed);
  st.compaction_bytes_written =
      compaction_metrics_.bytes_written.load(std::memory_order_relaxed);
  st.compaction_cpu_nanos =
      compaction_metrics_.cpu_nanos.load(std::memory_order_relaxed);
  st.total_lookups = total_lookups_.load(std::memory_order_relaxed);
  st.sstable_files_touched =
      sstable_files_touched_.load(std::memory_order_relaxed);
  st.write_stall_count = write_stall_count_.load(std::memory_order_relaxed);

  Version* v = const_cast<VersionSet&>(versions_).AcquireCurrentVersion();
  st.num_active_levels = v->ActiveLevelCount();
  v->Unref();
  return st;
}

Status DBImpl::Close() {
  bool expected = false;
  // Acquire-release CAS ensures Close() runs only once.
  if (!closed_.compare_exchange_strong(expected, true,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
    return Status::OK();
  }

  ForceFlush();
  if (compaction_pool_) {
    compaction_pool_->WaitForAll();
    compaction_pool_->Shutdown();
  }

  {
    std::lock_guard<std::mutex> lock(flush_mu_);
    stop_flush_.store(true, std::memory_order_release);
  }
  flush_cv_.notify_all();
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }

  MemTable* active = active_mem_.exchange(nullptr, std::memory_order_acq_rel);
  if (active != nullptr) {
    active->Unref();
  }
  {
    std::lock_guard<std::mutex> lock(flush_mu_);
    for (MemTable* m : retired_memtables_) {
      m->Unref();
    }
    retired_memtables_.clear();
  }
  if (current_wal_holder_) {
    current_wal_holder_->Sync();
    current_wal_holder_->Close();
    current_wal_holder_.reset();
  }
  return Status::OK();
}

}  // namespace lsm
