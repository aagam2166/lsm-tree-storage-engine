#ifndef LSM_SRC_UTIL_ZIPF_H_
#define LSM_SRC_UTIL_ZIPF_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lsm/types.h"

namespace lsm {

struct RankProbabilityRow {
  std::size_t rank = 0;
  double zipf_measured = 0.0;
  double uniform_measured = 0.0;
};

struct WorkloadConfig {
  std::size_t num_keys = 1000;
  double write_ratio = 0.9;
  KeyDistribution distribution = KeyDistribution::UNIFORM;
  double zipf_exponent = 1.0;
  uint64_t base_seed = 0;
};

enum class WorkloadOpType : uint8_t {
  kPut = 0,
  kGet = 1,
};

struct WorkloadOp {
  WorkloadOpType type = WorkloadOpType::kPut;
  std::string key;
  std::string value;
};

class WorkloadGenerator {
 public:
  WorkloadGenerator(const WorkloadConfig& config, uint32_t thread_id = 0);

  WorkloadOp NextOp();

  static std::vector<RankProbabilityRow> MeasureRankProbabilities(
      std::size_t num_keys, double exponent, std::size_t num_samples,
      std::size_t top_ranks, uint64_t seed);
  static std::string FormatSlide3Table(
      const std::vector<RankProbabilityRow>& rows,
      const std::string& output_path = "");

 private:
  std::size_t GenerateZipfRank() const;
  std::size_t GenerateUniformRank() const;

  WorkloadConfig config_;
  uint32_t thread_id_;
  uint64_t state_;
};

}  // namespace lsm

#endif  // LSM_SRC_UTIL_ZIPF_H_
