#ifndef LSM_SRC_DB_DB_IMPL_H_
#define LSM_SRC_DB_DB_IMPL_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "compaction/compaction.h"
#include "lsm/db.h"
#include "lsm/memtable.h"
#include "version/version.h"
#include "wal/wal.h"

namespace lsm {

class DBImpl final : public DB {
 public:
  DBImpl(const std::string& path, const Options& options);
  ~DBImpl() override;

  Status Initialize();

  Status Put(std::string_view key, std::string_view value) override;
  Status Delete(std::string_view key) override;
  Status Get(std::string_view key, std::string* value) override;
  Status Scan(std::string_view start_key, std::size_t limit,
              std::vector<std::pair<std::string, std::string>>* results) override;
  Status Close() override;

  Status ForceFlush() override;
  Status WaitForCompaction() override;
  DBStats GetStats() const override;
  uint64_t LastSequence() const noexcept override {
    return versions_.LastSequence();
  }

 private:
  Status WriteImpl(ValueType type, std::string_view key,
                   std::string_view value);
  void TryRotateMemTable(MemTable* full_mem);
  void EnqueueImmutableForFlush(MemTable* imm,
                                std::shared_ptr<WalWriter> old_wal);
  void FlushWorkerLoop();
  Status FlushMemTableToL0(MemTable* imm, uint64_t wal_seg_id);
  Status RecoverFromDisk();

  std::string db_path_;
  Options options_;
  VersionSet versions_;
  DisjointRangeRegistry range_registry_;
  CompactionMetrics compaction_metrics_;
  std::unique_ptr<CompactionPool> compaction_pool_;

  // Active MemTable and active WAL pointer.
  // Active MemTable is swapped via single atomic CAS (Section 3.3).
  std::atomic<MemTable*> active_mem_;
  std::atomic<WalWriter*> active_wal_;
  std::atomic<uint64_t> next_wal_segment_id_;

  // Immutable MemTables awaiting flush (read by Get() and consumed by flush worker)
  mutable std::mutex flush_mu_;
  std::condition_variable flush_cv_;
  std::condition_variable flush_done_cv_;
  std::vector<MemTable*> immutable_list_;
  std::vector<MemTable*> retired_memtables_;
  std::vector< std::shared_ptr<WalWriter> > retired_wals_;
  std::shared_ptr<WalWriter> current_wal_holder_;
  std::atomic<std::size_t> immutable_count_;
  std::atomic<bool> stop_flush_;
  std::atomic<bool> closed_;
  std::thread flush_thread_;

  // Engine telemetry counters (Section 5)
  std::atomic<uint64_t> fg_bytes_written_;
  std::atomic<uint64_t> total_lookups_;
  std::atomic<uint64_t> sstable_files_touched_;
  std::atomic<uint64_t> write_stall_count_;
};

}  // namespace lsm

#endif  // LSM_SRC_DB_DB_IMPL_H_
