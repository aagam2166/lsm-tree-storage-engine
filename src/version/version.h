#ifndef LSM_SRC_VERSION_VERSION_H_
#define LSM_SRC_VERSION_VERSION_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lsm/options.h"
#include "lsm/status.h"
#include "sstable/sstable.h"

namespace lsm {

struct FileMetaData {
  uint64_t file_number = 0;
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  InternalKey smallest;
  InternalKey largest;
  uint64_t max_seq = 0;
  std::shared_ptr<SSTableReader> reader;
};

struct VersionEdit {
  uint64_t next_file_number = 0;
  uint64_t last_sequence = 0;
  uint64_t min_log_number = 0;
  bool has_next_file_number = false;
  bool has_last_sequence = false;
  bool has_min_log_number = false;

  // (level, FileMetaData)
  std::vector<std::pair<int, FileMetaData>> added_files;
  // (level, file_number)
  std::vector<std::pair<int, uint64_t>> deleted_files;
};

class VersionSet;

// Immutable, reference-counted snapshot of live SSTables per level (Sec 3.6).
class Version {
 public:
  explicit Version(std::size_t max_levels, VersionSet* vset = nullptr);
  ~Version();

  void Ref() noexcept {
    // Relaxed fetch_add: caller already holds a valid reference.
    refs_.fetch_add(1, std::memory_order_relaxed);
  }

  void Unref() noexcept;

  const std::vector<FileMetaData>& LevelFiles(std::size_t level) const {
    return levels_[level];
  }

  std::size_t NumLevels() const noexcept { return levels_.size(); }
  std::size_t ActiveLevelCount() const noexcept;
  uint64_t LevelBytes(std::size_t level) const noexcept;

  // Point lookup across L0 (newest first) -> L1 -> ... -> Ln.
  // Increments `*files_touched` for each SSTable queried (Section 3.8).
  Status Get(std::string_view user_key, bool* found, std::string* value,
             ValueType* type, uint64_t* seq, uint64_t* files_touched) const;

 private:
  friend class VersionSet;
  std::atomic<int32_t> refs_;
  VersionSet* vset_;
  std::vector<std::vector<FileMetaData>> levels_;
};

class ManifestLog {
 public:
  explicit ManifestLog(const std::string& db_path);
  ~ManifestLog();

  Status OpenOrRecover(std::vector<VersionEdit>* edits_out);
  Status AppendEdit(const VersionEdit& edit);

 private:
  std::string db_path_;
  std::string manifest_filename_;
  int fd_;
};

class VersionSet {
 public:
  VersionSet(const std::string& db_path, const Options& options);
  ~VersionSet();

  Status Recover();

  // Acquires a reference to the current immutable Version via atomic load.
  Version* AcquireCurrentVersion() noexcept;

  // Atomically installs `edit` by constructing a new immutable Version and
  // swapping `current_` via CAS.
  Status LogAndApply(const VersionEdit& edit);

  uint64_t AllocateFileNumber() noexcept {
    // Relaxed fetch_add: unique monotonic file numbers.
    return next_file_number_.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t LastSequence() const noexcept {
    // Acquire load pairs with SetLastSequence release store.
    return last_sequence_.load(std::memory_order_acquire);
  }

  void SetLastSequence(uint64_t seq) noexcept {
    // Acquire load + CAS ensures last_sequence_ only moves forward.
    uint64_t cur = last_sequence_.load(std::memory_order_acquire);
    while (seq > cur && !last_sequence_.compare_exchange_weak(
                            cur, seq, std::memory_order_release,
                            std::memory_order_acquire)) {
    }
  }

  uint64_t FetchAddSequence() noexcept {
    // Acquire-release fetch_add provides globally ordered sequence numbers.
    return last_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  std::string SSTableFilePath(uint64_t file_number) const;
  std::string WalFilePath(uint64_t segment_id) const;

  void OnVersionDestroyed(const Version* v);
  void CleanupObsoleteFiles();

 private:
  Status ApplyEditToBuilder(const Version* base, const VersionEdit& edit,
                            Version* dst, bool open_readers = true);

  std::string db_path_;
  Options options_;
  ManifestLog manifest_;
  std::atomic<Version*> current_;
  std::atomic<uint64_t> next_file_number_;
  std::atomic<uint64_t> last_sequence_;
  std::atomic<uint64_t> min_log_number_;
  mutable std::recursive_mutex install_mu_;  // Serializes MANIFEST file appends & GC tracking only
  std::vector<Version*> live_versions_;
  std::set<uint64_t> all_known_sstables_;
};

}  // namespace lsm

#endif  // LSM_SRC_VERSION_VERSION_H_
