#ifndef LSM_SRC_MEMTABLE_MEMTABLE_IFACE_H_
#define LSM_SRC_MEMTABLE_MEMTABLE_IFACE_H_

#include <atomic>
#include <cassert>
#include "lsm/memtable.h"

namespace lsm {

class MemTableBase : public MemTable {
 public:
  MemTableBase() noexcept
      : refs_(1),
        in_flight_writers_(0),
        swap_claimed_(false),
        wal_segment_id_(0) {}

  ~MemTableBase() override = default;

  void Ref() noexcept override {
    // Relaxed fetch_add: increasing an already positive reference count does
    // not need to publish new state to other threads.
    refs_.fetch_add(1, std::memory_order_relaxed);
  }

  void Unref() noexcept override {
    // Release-acquire semantics on reference decrement ensures all prior
    // reads/writes by this thread happen-before the destructor runs.
    int32_t prev = refs_.fetch_sub(1, std::memory_order_acq_rel);
    assert(prev >= 1);
    if (prev == 1) {
      delete this;
    }
  }

  void BeginWrite() noexcept override {
    // Acquire-release ensures writer entrance is visible to the flush worker
    // before the writer mutates the skip list.
    in_flight_writers_.fetch_add(1, std::memory_order_acq_rel);
  }

  void EndWrite() noexcept override {
    // Release semantics ensures all skip list writes by this thread are visible
    // before the flush worker observes in_flight_writers_ == 0.
    in_flight_writers_.fetch_sub(1, std::memory_order_release);
  }

  uint64_t InFlightWriters() const noexcept override {
    // Acquire load pairs with EndWrite()'s release fetch_sub so the flush
    // worker sees all completed skip list mutations.
    return in_flight_writers_.load(std::memory_order_acquire);
  }

  bool TryClaimSwap() noexcept override {
    bool expected = false;
    // Acquire-release CAS ensures exactly one writer claims the MemTable swap.
    return swap_claimed_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
  }

  uint64_t GetWalSegmentId() const noexcept override {
    // Acquire load pairs with SetWalSegmentId()'s release store.
    return wal_segment_id_.load(std::memory_order_acquire);
  }

  void SetWalSegmentId(uint64_t id) noexcept override {
    // Release store publishes segment ID before MemTable becomes visible.
    wal_segment_id_.store(id, std::memory_order_release);
  }

 private:
  std::atomic<int32_t> refs_;
  std::atomic<uint64_t> in_flight_writers_;
  std::atomic<bool> swap_claimed_;
  std::atomic<uint64_t> wal_segment_id_;
};

}  // namespace lsm

#endif  // LSM_SRC_MEMTABLE_MEMTABLE_IFACE_H_
