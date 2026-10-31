#include "bench/metrics.h"

#include <fstream>
#include <iomanip>

namespace lsm {

std::string MetricsReporter::MemTableTypeName(MemTableType t) {
  switch (t) {
    case MemTableType::LOCK_FREE:
      return "LOCK_FREE";
    case MemTableType::GLOBAL_LOCK:
      return "GLOBAL_LOCK";
    case MemTableType::SHARDED:
      return "SHARDED";
  }
  return "LOCK_FREE";
}

std::string MetricsReporter::DistributionName(KeyDistribution d) {
  return d == KeyDistribution::ZIPFIAN ? "ZIPFIAN" : "UNIFORM";
}

void MetricsReporter::WriteCsvHeader(const std::string& csv_path) {
  std::ofstream out(csv_path, std::ios::trunc);
  out << "memtable,distribution,threads,trial,elapsed_sec,total_ops,write_ops,"
         "read_ops,throughput_ops_sec,write_ops_sec,p50_write_us,p99_write_us,"
         "p999_write_us,fg_bytes_written,compaction_bytes_read,"
         "compaction_bytes_written,compaction_cpu_nanos,compaction_io_ratio,"
         "write_amplification,total_lookups,sstable_files_touched,"
         "read_amplification,active_levels,write_stalls\n";
}

void MetricsReporter::AppendCsvRow(const std::string& csv_path,
                                   const BenchmarkRunResult& r) {
  std::ofstream out(csv_path, std::ios::app);
  out << MetricsReporter::MemTableTypeName(r.memtable_type) << ","
      << MetricsReporter::DistributionName(r.distribution) << ","
      << r.num_threads << "," << r.trial_id << "," << std::fixed
      << std::setprecision(6) << r.elapsed_sec << "," << r.total_ops << ","
      << r.write_ops << "," << r.read_ops << "," << std::setprecision(2)
      << r.throughput_ops_per_sec << "," << r.write_ops_per_sec << ","
      << std::setprecision(3) << r.p50_write_latency_us << ","
      << r.p99_write_latency_us << "," << r.p999_write_latency_us << ","
      << r.fg_bytes_written << "," << r.compaction_bytes_read << ","
      << r.compaction_bytes_written << "," << r.compaction_cpu_nanos << ","
      << std::setprecision(4) << r.compaction_io_ratio << ","
      << r.write_amplification << "," << r.total_lookups << ","
      << r.sstable_files_touched << "," << r.read_amplification << ","
      << r.num_active_levels << "," << r.write_stall_count << "\n";
}

}  // namespace lsm
