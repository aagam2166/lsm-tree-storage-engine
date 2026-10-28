#include "src/util/thread_pool.h"

#include <utility>

namespace lsm {

ThreadPool::ThreadPool(std::size_t threads) {
  workers_.reserve(threads == 0 ? 1 : threads);
  for (std::size_t i = 0; i < (threads == 0 ? 1 : threads); ++i) {
    workers_.emplace_back(&ThreadPool::Worker, this);
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

void ThreadPool::Worker() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
      if (stop_ && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

void ThreadPool::Join() {
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

}  // namespace lsm
