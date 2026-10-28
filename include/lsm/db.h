#ifndef LSM_INCLUDE_LSM_DB_H_
#define LSM_INCLUDE_LSM_DB_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <utility>
#include <vector>

#include "lsm/options.h"
#include "lsm/status.h"

namespace lsm {

struct DBStats {
  uint64_t foreground_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t compaction_cpu_nanos = 0;
  uint64_t total_lookups = 0;
  uint64_t sstable_files_touched = 0;
  uint64_t write_stall_count = 0;
  uint64_t num_active_levels = 0;
};

class DB {
 public:
  virtual ~DB() = default;

  static Status Open(const std::string& path, const Options& options,
                     std::unique_ptr<DB>* db_out);

  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;
  virtual Status Get(std::string_view key, std::string* value) = 0;
  virtual Status Scan(std::string_view start_key, std::size_t limit,
                      std::vector<std::pair<std::string, std::string>>* results) = 0;
  virtual Status Close() = 0;

  // Testing and benchmark helpers
  virtual Status ForceFlush() = 0;
  virtual Status WaitForCompaction() = 0;
  virtual DBStats GetStats() const = 0;
  virtual uint64_t LastSequence() const noexcept = 0;
};

}  // namespace lsm

#endif  // LSM_INCLUDE_LSM_DB_H_
