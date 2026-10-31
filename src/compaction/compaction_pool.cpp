#include "compaction/compaction.h"

#include <chrono>
#include <thread>

namespace lsm {

CompactionPool::CompactionPool(std::size_t num_workers, VersionSet* vset,
                               const Options& options,
                               DisjointRangeRegistry* registry,
                               CompactionMetrics* metrics)
    : num_workers_(num_workers == 0 ? 1 : num_workers),
      vset_(vset),
      options_(options),
      registry_(registry),
      metrics_(metrics),
      picker_(options, registry),
      stop_(false),
      pending_signals_(0),
      active_jobs_(0) {
  workers_.reserve(num_workers_);
  for (std::size_t i = 0; i < num_workers_; ++i) {
    workers_.emplace_back(&CompactionPool::WorkerEntry, this);
  }
}

CompactionPool::~CompactionPool() { Shutdown(); }

void CompactionPool::MaybeScheduleCompaction() {
  // Release fetch_add signals background compaction workers without locking.
  pending_signals_.fetch_add(1, std::memory_order_release);
}

void CompactionPool::WaitForAll() {
  while (true) {
    // Acquire loads check whether any compaction work remains.
    if (pending_signals_.load(std::memory_order_acquire) == 0 &&
        active_jobs_.load(std::memory_order_acquire) == 0) {
      Version* v = vset_->AcquireCurrentVersion();
      CompactionTask probe;
      bool has_work = picker_.PickCompaction(v, &probe);
      if (has_work) {
        registry_->ReleaseRange(probe.range_slot);
        probe.base_version->Unref();
        v->Unref();
        MaybeScheduleCompaction();
      } else {
        v->Unref();
        if (active_jobs_.load(std::memory_order_acquire) == 0) {
          break;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

void CompactionPool::Shutdown() {
  bool expected = false;
  // Acquire-release CAS ensures Shutdown runs at most once.
  if (!stop_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                     std::memory_order_relaxed)) {
    return;
  }
  for (auto& w : workers_) {
    if (w.joinable()) w.join();
  }
}

void CompactionPool::WorkerEntry() {
  while (!stop_.load(std::memory_order_acquire)) {
    uint64_t sig = pending_signals_.load(std::memory_order_acquire);
    if (sig == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(250));
      continue;
    }

    // Acquire-release increment marks worker active before decrementing signal.
    active_jobs_.fetch_add(1, std::memory_order_acq_rel);
    pending_signals_.compare_exchange_weak(sig, sig - 1,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed);

    Version* v = vset_->AcquireCurrentVersion();
    CompactionTask task;
    if (picker_.PickCompaction(v, &task)) {
      v->Unref();
      CompactionJob job(std::move(task), vset_, options_, registry_, metrics_);
      job.Run();
      // Trigger follow-up check in case cascading compaction is needed.
      pending_signals_.fetch_add(1, std::memory_order_release);
    } else {
      v->Unref();
    }

    // Release decrement signals job completion to WaitForAll().
    active_jobs_.fetch_sub(1, std::memory_order_release);
  }
}

}  // namespace lsm
