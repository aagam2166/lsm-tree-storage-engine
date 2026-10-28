#ifndef LSM_SRC_UTIL_HISTOGRAM_H_
#define LSM_SRC_UTIL_HISTOGRAM_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lsm {

class Histogram {
 public:
  Histogram() = default;

  void Record(uint64_t value);
  void Merge(const Histogram& other);

  uint64_t Count() const noexcept;
  uint64_t Min() const noexcept;
  uint64_t Max() const noexcept;
  double Percentile(double pct) const;

 private:
  std::vector<uint64_t> samples_;
  uint64_t count_ = 0;
};

}  // namespace lsm

#endif  // LSM_SRC_UTIL_HISTOGRAM_H_
