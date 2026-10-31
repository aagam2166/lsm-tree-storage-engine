#include "src/util/histogram.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lsm {

void Histogram::Record(uint64_t value) {
  samples_.push_back(value);
  count_ += 1;
}

void Histogram::Merge(const Histogram& other) {
  samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
  count_ += other.count_;
}

uint64_t Histogram::Count() const noexcept { return count_; }

uint64_t Histogram::Min() const noexcept {
  if (samples_.empty()) return 0;
  return *std::min_element(samples_.begin(), samples_.end());
}

uint64_t Histogram::Max() const noexcept {
  if (samples_.empty()) return 0;
  return *std::max_element(samples_.begin(), samples_.end());
}

double Histogram::Percentile(double pct) const {
  if (samples_.empty()) return 0.0;
  if (pct <= 0.0) return static_cast<double>(Min());
  if (pct >= 100.0) return static_cast<double>(Max());

  std::vector<uint64_t> copy = samples_;
  std::sort(copy.begin(), copy.end());
  const double rank = (copy.size() - 1) * (pct / 100.0);
  const std::size_t lower = static_cast<std::size_t>(std::floor(rank));
  const std::size_t upper = static_cast<std::size_t>(std::ceil(rank));
  const double frac = rank - static_cast<double>(lower);
  const double value = static_cast<double>(copy[lower]);
  const double upper_value = static_cast<double>(copy[std::min(upper, copy.size() - 1)]);
  return value + (upper_value - value) * frac;
}

}  // namespace lsm
