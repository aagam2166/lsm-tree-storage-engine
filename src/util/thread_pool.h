#ifndef LSM_SRC_UTIL_THREAD_POOL_H_
#define LSM_SRC_UTIL_THREAD_POOL_H_

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace lsm {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t threads = 1);
  ~ThreadPool();

  template <typename F>
  void Submit(F&& task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;
      tasks_.emplace(std::forward<F>(task));
    }
    cv_.notify_one();
  }

  void Join();

 private:
  void Worker();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  bool stop_ = false;
};

}  // namespace lsm

#endif  // LSM_SRC_UTIL_THREAD_POOL_H_
