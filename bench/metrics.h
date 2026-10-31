#ifndef LSM_BENCH_METRICS_H_
#define LSM_BENCH_METRICS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lsm/db.h"
#include "lsm/options.h"
#include "util/histogram.h"

namespace lsm {

struct BenchmarkRunResult {
  MemTableType memtable_type;
  KeyDistribution distribution;
  std::size_t num_threads = 1;
  std::size_t trial_id = 0;

  double elapsed_sec = 0.0;
  uint64_t total_ops = 0;
  uint64_t write_ops = 0;
  uint64_t read_ops = 0;

  // Metric 1: Write throughput (completed ops/sec)
  double throughput_ops_per_sec = 0.0;
  double write_ops_per_sec = 0.0;

  // Metric 2: Write latencies (ns -> us) from merged thread-local HDR histograms
  double p50_write_latency_us = 0.0;
  double p99_write_latency_us = 0.0;
  double p999_write_latency_us = 0.0;

  // Metric 3: Compaction overhead (CPU time & I/O bytes vs foreground writes)
  uint64_t fg_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t compaction_cpu_nanos = 0;
  double compaction_io_ratio = 0.0;
  double write_amplification = 1.0;

  // Metric 4: Read amplification (average SSTables touched per lookup)
  uint64_t total_lookups = 0;
  uint64_t sstable_files_touched = 0;
  double read_amplification = 0.0;
  uint64_t num_active_levels = 0;
  uint64_t write_stall_count = 0;
};

class MetricsReporter {
 public:
  static std::string MemTableTypeName(MemTableType t);
  static std::string DistributionName(KeyDistribution d);
  static void WriteCsvHeader(const std::string& csv_path);
  static void AppendCsvRow(const std::string& csv_path,
                           const BenchmarkRunResult& r);
};

}  // namespace lsm

#endif  // LSM_BENCH_METRICS_H_
