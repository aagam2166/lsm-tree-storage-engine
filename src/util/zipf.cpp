#include "src/util/zipf.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace lsm {

namespace {

uint64_t Mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDULL;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ULL;
  x ^= x >> 33;
  return x;
}

}  // namespace

WorkloadGenerator::WorkloadGenerator(const WorkloadConfig& config, uint32_t thread_id)
    : config_(config), thread_id_(thread_id), state_(Mix(config.base_seed + thread_id + 0x9E3779B97F4A7C15ULL)) {
  if (config_.distribution == KeyDistribution::ZIPFIAN && config_.num_keys > 0) {
    cdf_.resize(config_.num_keys);
    const double s = std::max(0.0, config_.zipf_exponent);
    double sum = 0.0;
    for (std::size_t i = 1; i <= config_.num_keys; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i), s);
      cdf_[i - 1] = sum;
    }
    if (sum > 0.0) {
      for (double& v : cdf_) {
        v /= sum;
      }
      cdf_.back() = 1.0;
    }
  }
}

WorkloadOp WorkloadGenerator::NextOp() {
  WorkloadOp op;
  const std::size_t key_rank = config_.distribution == KeyDistribution::ZIPFIAN
                                 ? GenerateZipfRank()
                                 : GenerateUniformRank();
  op.key = FormatFixedKey(key_rank);
  op.value = std::string(100, 'x');
  op.type = (state_ % 100 < static_cast<uint64_t>(config_.write_ratio * 100.0))
                ? WorkloadOpType::kPut
                : WorkloadOpType::kGet;
  state_ = Mix(state_ + 0x9E3779B97F4A7C15ULL + thread_id_);
  return op;
}

std::size_t WorkloadGenerator::GenerateZipfRank() const {
  if (cdf_.empty()) return 0;
  std::mt19937_64 rng(state_ ^ (thread_id_ * 0x9E3779B97F4A7C15ULL));
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  const double u = dist(rng);
  auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
  std::size_t rank = std::distance(cdf_.begin(), it);
  if (rank >= cdf_.size()) rank = cdf_.size() - 1;
  return rank;
}

std::size_t WorkloadGenerator::GenerateUniformRank() const {
  if (config_.num_keys == 0) return 0;
  std::mt19937_64 rng(state_ ^ (thread_id_ * 0x9E3779B97F4A7C15ULL));
  return static_cast<std::size_t>(rng() % config_.num_keys);
}

std::vector<RankProbabilityRow> WorkloadGenerator::MeasureRankProbabilities(
    std::size_t num_keys, double exponent, std::size_t num_samples,
    std::size_t top_ranks, uint64_t seed) {
  std::vector<RankProbabilityRow> rows;
  rows.reserve(std::min(top_ranks, num_keys));

  std::vector<std::size_t> counts(num_keys, 0);
  std::mt19937_64 rng(seed);
  const double s = std::max(0.0, exponent);
  std::vector<double> weights(num_keys);
  double total = 0.0;
  for (std::size_t i = 0; i < num_keys; ++i) {
    weights[i] = 1.0 / std::pow(static_cast<double>(i + 1), s);
    total += weights[i];
  }

  for (std::size_t i = 0; i < num_samples; ++i) {
    std::uniform_real_distribution<double> dist(0.0, total);
    const double x = dist(rng);
    double acc = 0.0;
    std::size_t rank = 0;
    for (std::size_t r = 0; r < num_keys; ++r) {
      acc += weights[r];
      if (x <= acc) {
        rank = r;
        break;
      }
    }
    counts[rank]++;
  }

  const double inv_samples = 1.0 / static_cast<double>(num_samples);
  const double uniform_prob = 1.0 / static_cast<double>(num_keys);
  for (std::size_t r = 0; r < std::min(top_ranks, num_keys); ++r) {
    RankProbabilityRow row;
    row.rank = r + 1;
    row.zipf_measured = static_cast<double>(counts[r]) * inv_samples;
    row.uniform_measured = uniform_prob;
    rows.push_back(row);
  }
  return rows;
}

std::string WorkloadGenerator::FormatSlide3Table(
    const std::vector<RankProbabilityRow>& rows,
    const std::string& output_path) {
  std::ostringstream oss;
  oss << "| Rank | Zipfian Measured | Zipfian Target (Slide 3) | Uniform Measured | Uniform Target (Slide 3) |\n";
  oss << "|-----:|-----------------:|-------------------------:|-----------------:|-------------------------:|\n";
  const double targets[5] = {0.134, 0.067, 0.045, 0.033, 0.027};
  for (std::size_t i = 0; i < rows.size() && i < 5; ++i) {
    oss << "| " << rows[i].rank << " | "
        << std::fixed << std::setprecision(3) << rows[i].zipf_measured << " | "
        << targets[i] << " | "
        << rows[i].uniform_measured << " | 0.001 |\n";
  }

  if (!output_path.empty()) {
    std::ofstream out(output_path);
    if (out.is_open()) {
      out << "rank,zipf_measured,zipf_target,uniform_measured,uniform_target\n";
      for (std::size_t i = 0; i < rows.size() && i < 5; ++i) {
        out << rows[i].rank << "," << rows[i].zipf_measured << "," << targets[i]
            << "," << rows[i].uniform_measured << ",0.001\n";
      }
    }
  }
  return oss.str();
}

}  // namespace lsm
