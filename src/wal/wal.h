#ifndef LSM_SRC_WAL_WAL_H_
#define LSM_SRC_WAL_WAL_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/memtable.h"
#include "lsm/options.h"
#include "lsm/status.h"
#include "lsm/types.h"

namespace lsm {

// Record layout (Section 3.4):
// [crc32c: 4B][length: 4B][seq: 8B][type: 1B][key: 16B][value: length B]
constexpr std::size_t kWalHeaderSize = 4 + 4 + 8 + 1 + kDefaultKeySize;  // 33B

class WalWriter {
 public:
  WalWriter(const std::string& path, uint64_t segment_id, SyncPolicy policy,
            uint64_t group_commit_interval_us = 1000);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status Open();
  Status AppendRecord(uint64_t seq, ValueType type, std::string_view key,
                      std::string_view value);
  Status Sync();
  Status Close();

  uint64_t segment_id() const noexcept { return segment_id_; }
  const std::string& path() const noexcept { return path_; }
  uint64_t BytesWritten() const noexcept {
    // Relaxed load for metrics reporting.
    return log_offset_.load(std::memory_order_relaxed);
  }

 private:
  std::string path_;
  uint64_t segment_id_;
  SyncPolicy policy_;
  uint64_t group_commit_interval_us_;
  int fd_;
  std::atomic<uint64_t> log_offset_;
  std::atomic<uint64_t> last_sync_micros_;
  std::atomic<bool> sync_in_progress_;
};

class WalReader {
 public:
  explicit WalReader(const std::string& path);

  // Replays valid records into `memtable`, updates `*max_seq`, and stops
  // cleanly at the first torn or corrupt record at the WAL tail (Section 3.4).
  Status ReplayInto(MemTable* memtable, uint64_t* max_seq,
                    std::size_t* records_replayed = nullptr);

 private:
  std::string path_;
};

}  // namespace lsm

#endif  // LSM_SRC_WAL_WAL_H_
