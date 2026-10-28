#ifndef LSM_SRC_UTIL_CLOCK_H_
#define LSM_SRC_UTIL_CLOCK_H_

#include <chrono>
#include <cstdint>
#include <ctime>

namespace lsm {

class Clock {
 public:
  static uint64_t NowMicros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  }

  static uint64_t NowNanos() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  }

  static uint64_t ThreadCpuNanos() {
    ::timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
      return NowNanos();
    }
    return (static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL) +
           static_cast<uint64_t>(ts.tv_nsec);
  }
};

}  // namespace lsm

#endif  // LSM_SRC_UTIL_CLOCK_H_
