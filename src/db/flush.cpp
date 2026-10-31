#include "db/db_impl.h"

#include <unistd.h>
#include <thread>

namespace lsm {

void DBImpl::EnqueueImmutableForFlush(MemTable* imm,
                                      std::shared_ptr<WalWriter> new_wal) {
  {
    std::lock_guard<std::mutex> lock(flush_mu_);
    retired_wals_.push_back(std::move(current_wal_holder_));
    current_wal_holder_ = std::move(new_wal);
    immutable_list_.push_back(imm);
    // Release fetch_add updates immutable MemTable count for backpressure check.
    immutable_count_.fetch_add(1, std::memory_order_release);
  }
  flush_cv_.notify_one();
}

void DBImpl::FlushWorkerLoop() {
  while (true) {
    MemTable* imm = nullptr;
    std::shared_ptr<WalWriter> old_wal;
    {
      std::unique_lock<std::mutex> lock(flush_mu_);
      flush_cv_.wait(lock, [this]() {
        return stop_flush_.load(std::memory_order_acquire) ||
               !immutable_list_.empty();
      });
      if (immutable_list_.empty() &&
          stop_flush_.load(std::memory_order_acquire)) {
        return;
      }
      imm = immutable_list_.front();
      if (!retired_wals_.empty()) {
        old_wal = retired_wals_.front();
        retired_wals_.erase(retired_wals_.begin());
      }
    }

    // Section 3.3: The FLUSH worker (not the writers) waits until the frozen
    // MemTable has zero in-flight writers before flushing it.
    while (imm->InFlightWriters() > 0) {
      std::this_thread::yield();
    }

    if (old_wal) {
      old_wal->Sync();
      old_wal->Close();
    }

    uint64_t wal_seg_id = imm->GetWalSegmentId();
    FlushMemTableToL0(imm, wal_seg_id);

    {
      std::lock_guard<std::mutex> lock(flush_mu_);
      if (!immutable_list_.empty() && immutable_list_.front() == imm) {
        immutable_list_.erase(immutable_list_.begin());
      }
      // Release fetch_sub decrements immutable MemTable counter.
      immutable_count_.fetch_sub(1, std::memory_order_release);
      retired_memtables_.push_back(imm);
    }
    flush_done_cv_.notify_all();

    if (compaction_pool_) {
      compaction_pool_->MaybeScheduleCompaction();
    }
  }
}

Status DBImpl::FlushMemTableToL0(MemTable* imm, uint64_t wal_seg_id) {
  auto iter = imm->NewIterator();
  iter->SeekToFirst();
  if (!iter->Valid()) {
    ::unlink(versions_.WalFilePath(wal_seg_id).c_str());
    return Status::OK();
  }

  uint64_t file_num = versions_.AllocateFileNumber();
  std::string sst_path = versions_.SSTableFilePath(file_num);
  SSTableBuilder builder(sst_path, file_num, options_);

  uint64_t count = 0;
  for (; iter->Valid(); iter->Next()) {
    Status s = builder.Add(iter->key(), iter->value());
    if (!s.ok()) return s;
    ++count;
  }

  if (count == 0) {
    ::unlink(sst_path.c_str());
    ::unlink(versions_.WalFilePath(wal_seg_id).c_str());
    return Status::OK();
  }

  SSTableMeta sst_meta{};
  Status s = builder.Finish(&sst_meta);
  if (!s.ok()) return s;

  VersionEdit edit;
  FileMetaData meta;
  meta.file_number = sst_meta.file_number;
  meta.file_size = sst_meta.file_size;
  meta.num_entries = sst_meta.num_entries;
  meta.smallest = sst_meta.smallest;
  meta.largest = sst_meta.largest;
  meta.max_seq = sst_meta.max_seq;
  edit.added_files.emplace_back(0, std::move(meta));
  edit.has_min_log_number = true;
  edit.min_log_number = wal_seg_id + 1;

  s = versions_.LogAndApply(edit);
  if (s.ok()) {
    // Section 3.4: Delete WAL segment only after its SSTable is durably
    // installed in the MANIFEST.
    ::unlink(versions_.WalFilePath(wal_seg_id).c_str());
  }
  return s;
}

Status DBImpl::ForceFlush() {
  MemTable* cur = active_mem_.load(std::memory_order_acquire);
  if (cur != nullptr && cur->ApproximateSize() > 0) {
    TryRotateMemTable(cur);
  }
  std::unique_lock<std::mutex> lock(flush_mu_);
  flush_done_cv_.wait(lock, [this]() { return immutable_list_.empty(); });
  return Status::OK();
}

Status DBImpl::WaitForCompaction() {
  ForceFlush();
  if (compaction_pool_) {
    compaction_pool_->MaybeScheduleCompaction();
    compaction_pool_->WaitForAll();
  }
  return Status::OK();
}

}  // namespace lsm
