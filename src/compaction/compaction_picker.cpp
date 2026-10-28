#include "compaction/compaction.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace lsm {

// ============================================================================
// Proof of Mutual Exclusion for DisjointRangeRegistry (Section 3.7):
//
// Each slot s in [0, kMaxSlots) has an atomic state in {kFree, kInitializing, kActive}.
// To claim a range R_i = [min_i, max_i]:
//   1. Thread i transitions a slot s_i from kFree -> kInitializing via CAS
//      (acq_rel) and writes min_i, max_i into slots_[s_i].
//   2. Thread i executes a sequential-consistency fence
//      (`std::atomic_thread_fence(std::memory_order_seq_cst)`).
//   3. Thread i scans all j != s_i:
//      - If slots_[j].state == kInitializing, there is a concurrent claim in
//        flight. To prevent the symmetric Dekker race where both threads miss
//        each other's kActive transition, Thread i immediately aborts its own
//        claim (kInitializing -> kFree) if any other slot is kInitializing, or
//        if any kActive slot overlaps R_i.
//   4. Only if all other slots are either kFree or (kActive and disjoint from
//      R_i) does Thread i transition slots_[s_i].state from kInitializing to
//      kActive (release).
//
// Invariant Proof:
// Suppose two threads T1 (slot s1, range R1) and T2 (slot s2, range R2) with
// overlapping ranges R1 ∩ R2 != ∅ both attempt TryClaimRange concurrently.
// Both write their state=kInitializing + range bounds and execute a `seq_cst`
// fence BEFORE scanning the array. In the total `seq_cst` order, one fence
// executes second (say T2's fence). When T2 scans slot s1, s1 is already in
// state kInitializing or kActive:
//   - If s1 is kInitializing, T2 sees kInitializing and aborts.
//   - If s1 already reached kActive, T2 reads s1's bounds and sees R1 ∩ R2 != ∅
//     and aborts.
// Therefore, two overlapping ranges can NEVER both reach kActive!
// ============================================================================

DisjointRangeRegistry::DisjointRangeRegistry() noexcept {
  for (std::size_t i = 0; i < kMaxSlots; ++i) {
    // Relaxed store in constructor.
    slots_[i].state.store(kFree, std::memory_order_relaxed);
    std::memset(slots_[i].min_key, 0, kDefaultKeySize);
    std::memset(slots_[i].max_key, 0, kDefaultKeySize);
  }
}

bool DisjointRangeRegistry::RangesOverlap(const char* a_min, const char* a_max,
                                          const char* b_min,
                                          const char* b_max) noexcept {
  // Two closed intervals [a_min, a_max] and [b_min, b_max] overlap iff
  // a_min <= b_max && b_min <= a_max.
  return std::memcmp(a_min, b_max, kDefaultKeySize) <= 0 &&
         std::memcmp(b_min, a_max, kDefaultKeySize) <= 0;
}

bool DisjointRangeRegistry::TryClaimRange(std::string_view min_key,
                                          std::string_view max_key,
                                          int* slot_out) noexcept {
  for (int retry = 0; retry < 8; ++retry) {
    int my_slot = -1;
    for (std::size_t i = 0; i < kMaxSlots; ++i) {
      uint8_t expected = kFree;
      // Acquire-release CAS claims an unused slot for initialization.
      if (slots_[i].state.compare_exchange_strong(
              expected, kInitializing, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        my_slot = static_cast<int>(i);
        break;
      }
    }
    if (my_slot < 0) return false;

    std::memset(slots_[my_slot].min_key, 0, kDefaultKeySize);
    std::memset(slots_[my_slot].max_key, 0, kDefaultKeySize);
    std::memcpy(slots_[my_slot].min_key, min_key.data(),
                min_key.size() < kDefaultKeySize ? min_key.size()
                                                 : kDefaultKeySize);
    std::memcpy(slots_[my_slot].max_key, max_key.data(),
                max_key.size() < kDefaultKeySize ? max_key.size()
                                                 : kDefaultKeySize);

    // Full seq_cst barrier establishes total order between writing our slot's
    // kInitializing state + range bounds and scanning all other slots.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    bool conflict = false;
    bool transient_contention = false;
    for (std::size_t j = 0; j < kMaxSlots; ++j) {
      if (static_cast<int>(j) == my_slot) continue;
      // Acquire load observes any kInitializing or kActive state and its bounds.
      uint8_t st = slots_[j].state.load(std::memory_order_acquire);
      if (st == kInitializing) {
        transient_contention = true;
        break;
      }
      if (st == kActive) {
        if (RangesOverlap(slots_[my_slot].min_key, slots_[my_slot].max_key,
                          slots_[j].min_key, slots_[j].max_key)) {
          conflict = true;
          break;
        }
      }
    }

    if (!conflict && !transient_contention) {
      // Release store transitions our slot to kActive after passing all checks.
      slots_[my_slot].state.store(kActive, std::memory_order_release);
      *slot_out = my_slot;
      return true;
    }

    // Release store resets slot to kFree on conflict or transient contention.
    slots_[my_slot].state.store(kFree, std::memory_order_release);
    if (conflict) {
      return false;
    }
  }
  return false;
}

void DisjointRangeRegistry::ReleaseRange(int slot) noexcept {
  if (slot < 0 || static_cast<std::size_t>(slot) >= kMaxSlots) return;
  // Release store frees the slot and makes it available to future claims.
  slots_[slot].state.store(kFree, std::memory_order_release);
}

std::size_t DisjointRangeRegistry::ActiveRangeCount() const noexcept {
  std::size_t c = 0;
  for (std::size_t i = 0; i < kMaxSlots; ++i) {
    // Acquire load for inspection.
    if (slots_[i].state.load(std::memory_order_acquire) == kActive) {
      ++c;
    }
  }
  return c;
}

CompactionPicker::CompactionPicker(const Options& options,
                                   DisjointRangeRegistry* registry)
    : options_(options), registry_(registry) {}

bool CompactionPicker::PickCompaction(Version* version,
                                      CompactionTask* task_out) {
  // 1. Check Level 0 file count score
  const auto& l0 = version->LevelFiles(0);
  if (l0.size() >= options_.l0_compaction_trigger) {
    InternalKey min_k = l0[0].smallest;
    InternalKey max_k = l0[0].largest;
    for (const auto& f : l0) {
      if (f.smallest.Compare(min_k) < 0) min_k = f.smallest;
      if (f.largest.Compare(max_k) > 0) max_k = f.largest;
    }

    std::vector<FileMetaData> l1_overlaps;
    if (version->NumLevels() > 1) {
      for (const auto& f : version->LevelFiles(1)) {
        if (std::memcmp(f.smallest.user_key, max_k.user_key, kDefaultKeySize) <= 0 &&
            std::memcmp(min_k.user_key, f.largest.user_key, kDefaultKeySize) <= 0) {
          l1_overlaps.push_back(f);
          if (f.smallest.Compare(min_k) < 0) min_k = f.smallest;
          if (f.largest.Compare(max_k) > 0) max_k = f.largest;
        }
      }
    }

    int slot = -1;
    if (registry_->TryClaimRange(min_k.UserKeyView(), max_k.UserKeyView(),
                                 &slot)) {
      task_out->input_level = 0;
      task_out->output_level = 1;
      task_out->range_slot = slot;
      task_out->min_user_key = min_k.UserKeyString();
      task_out->max_user_key = max_k.UserKeyString();
      task_out->level_inputs = l0;
      task_out->output_level_inputs = std::move(l1_overlaps);
      version->Ref();
      task_out->base_version = version;
      return true;
    }
  }

  // 2. Check Levels 1 .. max_levels - 2 by byte size score; can pick disjoint
  // individual files in parallel across workers!
  uint64_t target_bytes = options_.l1_max_bytes;
  for (std::size_t lvl = 1; lvl + 1 < version->NumLevels(); ++lvl) {
    if (version->LevelBytes(lvl) > target_bytes) {
      for (const auto& candidate : version->LevelFiles(lvl)) {
        InternalKey min_k = candidate.smallest;
        InternalKey max_k = candidate.largest;
        std::vector<FileMetaData> next_overlaps;
        for (const auto& f : version->LevelFiles(lvl + 1)) {
          if (std::memcmp(f.smallest.user_key, max_k.user_key,
                          kDefaultKeySize) <= 0 &&
              std::memcmp(min_k.user_key, f.largest.user_key,
                          kDefaultKeySize) <= 0) {
            next_overlaps.push_back(f);
            if (f.smallest.Compare(min_k) < 0) min_k = f.smallest;
            if (f.largest.Compare(max_k) > 0) max_k = f.largest;
          }
        }

        int slot = -1;
        if (registry_->TryClaimRange(min_k.UserKeyView(), max_k.UserKeyView(),
                                     &slot)) {
          task_out->input_level = static_cast<int>(lvl);
          task_out->output_level = static_cast<int>(lvl + 1);
          task_out->range_slot = slot;
          task_out->min_user_key = min_k.UserKeyString();
          task_out->max_user_key = max_k.UserKeyString();
          task_out->level_inputs = {candidate};
          task_out->output_level_inputs = std::move(next_overlaps);
          version->Ref();
          task_out->base_version = version;
          return true;
        }
      }
    }
    target_bytes *= options_.level_size_multiplier;
  }

  return false;
}

}  // namespace lsm
