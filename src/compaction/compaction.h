#ifndef LSM_SRC_COMPACTION_COMPACTION_H_
#define LSM_SRC_COMPACTION_COMPACTION_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include "lsm/options.h"
#include "lsm/status.h"
#include "version/version.h"

namespace lsm {

// Section 3.7 & 10: Lock-free CAS-based registry for claiming disjoint key
// ranges during parallel compaction. Zero mutexes exist in this registry.
class DisjointRangeRegistry {
 public:
  static constexpr std::size_t kMaxSlots = 64;

  DisjointRangeRegistry() noexcept;

  // Attempts to atomically claim `[min_key, max_key]` (16-byte user keys).
  // Returns true and sets `*slot_out` if and only if `[min_key, max_key]` does
  // not overlap with any currently claimed range.
  bool TryClaimRange(std::string_view min_key, std::string_view max_key,
                     int* slot_out) noexcept;

  void ReleaseRange(int slot) noexcept;

  std::size_t ActiveRangeCount() const noexcept;

 private:
  enum SlotState : uint8_t {
    kFree = 0,
    kInitializing = 1,
    kActive = 2
  };

  struct alignas(64) Slot {
    std::atomic<uint8_t> state;
    char min_key[kDefaultKeySize];
    char max_key[kDefaultKeySize];
  };

  static bool RangesOverlap(const char* a_min, const char* a_max,
                            const char* b_min, const char* b_max) noexcept;

  Slot slots_[kMaxSlots];
};

struct CompactionTask {
  int input_level = 0;
  int output_level = 1;
  int range_slot = -1;
  std::string min_user_key;
  std::string max_user_key;
  std::vector<FileMetaData> level_inputs;
  std::vector<FileMetaData> output_level_inputs;
  Version* base_version = nullptr;  // Holds a Ref() acquired by picker
};

class CompactionPicker {
 public:
  CompactionPicker(const Options& options, DisjointRangeRegistry* registry);

  // Inspects `version` and claims a non-overlapping compaction task if any
  // level exceeds its score threshold.
  bool PickCompaction(Version* version, CompactionTask* task_out);

 private:
  Options options_;
  DisjointRangeRegistry* registry_;
};

struct CompactionMetrics {
  std::atomic<uint64_t> bytes_read{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> cpu_nanos{0};
  std::atomic<uint64_t> jobs_completed{0};
};

class CompactionJob {
 public:
  CompactionJob(CompactionTask task, VersionSet* vset, const Options& options,
                DisjointRangeRegistry* registry, CompactionMetrics* metrics);
  ~CompactionJob();

  Status Run();

 private:
  CompactionTask task_;
  VersionSet* vset_;
  Options options_;
  DisjointRangeRegistry* registry_;
  CompactionMetrics* metrics_;
};

class CompactionPool {
 public:
  CompactionPool(std::size_t num_workers, VersionSet* vset,
                 const Options& options, DisjointRangeRegistry* registry,
                 CompactionMetrics* metrics);
  ~CompactionPool();

  void MaybeScheduleCompaction();
  void WaitForAll();
  void Shutdown();

 private:
  void WorkerEntry();

  std::size_t num_workers_;
  VersionSet* vset_;
  Options options_;
  DisjointRangeRegistry* registry_;
  CompactionMetrics* metrics_;
  CompactionPicker picker_;

  std::atomic<bool> stop_;
  std::atomic<uint64_t> pending_signals_;
  std::atomic<uint64_t> active_jobs_;
  std::vector<std::thread> workers_;
};

}  // namespace lsm

#endif  // LSM_SRC_COMPACTION_COMPACTION_H_
