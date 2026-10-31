#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "bench/metrics.h"
#include "bench/workload_gen.h"
#include "lsm/db.h"
#include "util/clock.h"
#include "util/histogram.h"

namespace lsm {
namespace {

BenchmarkRunResult RunSingleCell(MemTableType mt, KeyDistribution dist,
                                 std::size_t threads, std::size_t ops_per_thread,
                                 std::size_t trial, const std::string& base_dir) {
  std::string db_dir = base_dir + "/bench_db_" +
                       MetricsReporter::MemTableTypeName(mt) + "_" +
                       MetricsReporter::DistributionName(dist) + "_t" +
                       std::to_string(threads) + "_r" + std::to_string(trial);
  std::filesystem::remove_all(db_dir);

  Options opt;
  opt.memtable_type = mt;
  opt.memtable_max_size = 512 * 1024;  // 512 KB to exercise flushes & compaction
  opt.sharded_memtable_shards = 16;
  opt.sync_policy = SyncPolicy::NO_SYNC;
  opt.compaction_threads = 4;
  opt.l0_compaction_trigger = 4;
  opt.l1_max_bytes = 2 * 1024 * 1024;

  std::unique_ptr<DB> db;
  Status s = DB::Open(db_dir, opt, &db);
  if (!s.ok()) {
    std::cerr << "Failed to open DB: " << s.ToString() << "\n";
    std::exit(1);
  }

  WorkloadConfig wcfg;
  wcfg.num_keys = 20000;
  wcfg.write_ratio = 0.90;
  wcfg.distribution = dist;
  wcfg.zipf_exponent = 1.0;
  wcfg.base_seed = 100000ULL + trial * 7919ULL;

  std::vector<Histogram> thread_histograms(threads);
  std::vector<uint64_t> thread_writes(threads, 0);
  std::vector<uint64_t> thread_reads(threads, 0);
  std::atomic<bool> start_flag{false};

  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (std::size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      WorkloadGenerator gen(wcfg, static_cast<uint32_t>(t));
      // Spin until all threads are ready
      while (!start_flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t i = 0; i < ops_per_thread; ++i) {
        WorkloadOp op = gen.NextOp();
        if (op.type == WorkloadOpType::kPut) {
          uint64_t t0 = Clock::NowNanos();
          db->Put(op.key, op.value);
          uint64_t t1 = Clock::NowNanos();
          thread_histograms[t].Record(t1 - t0);
          thread_writes[t]++;
        } else {
          std::string val;
          db->Get(op.key, &val);
          thread_reads[t]++;
        }
      }
    });
  }

  uint64_t wall_start = Clock::NowNanos();
  // Release store starts all client threads simultaneously.
  start_flag.store(true, std::memory_order_release);
  for (auto& w : workers) {
    w.join();
  }
  uint64_t wall_end = Clock::NowNanos();

  db->WaitForCompaction();
  DBStats stats = db->GetStats();
  db->Close();
  std::filesystem::remove_all(db_dir);

  Histogram merged_hist;
  uint64_t total_w = 0, total_r = 0;
  for (std::size_t t = 0; t < threads; ++t) {
    merged_hist.Merge(thread_histograms[t]);
    total_w += thread_writes[t];
    total_r += thread_reads[t];
  }

  BenchmarkRunResult res{};
  res.memtable_type = mt;
  res.distribution = dist;
  res.num_threads = threads;
  res.trial_id = trial;
  res.elapsed_sec = static_cast<double>(wall_end - wall_start) / 1e9;
  res.total_ops = total_w + total_r;
  res.write_ops = total_w;
  res.read_ops = total_r;
  res.throughput_ops_per_sec =
      res.elapsed_sec > 0 ? static_cast<double>(res.total_ops) / res.elapsed_sec
                          : 0.0;
  res.write_ops_per_sec =
      res.elapsed_sec > 0 ? static_cast<double>(res.write_ops) / res.elapsed_sec
                          : 0.0;

  res.p50_write_latency_us =
      static_cast<double>(merged_hist.Percentile(50.0)) / 1000.0;
  res.p99_write_latency_us =
      static_cast<double>(merged_hist.Percentile(99.0)) / 1000.0;
  res.p999_write_latency_us =
      static_cast<double>(merged_hist.Percentile(99.9)) / 1000.0;

  res.fg_bytes_written = stats.foreground_bytes_written;
  res.compaction_bytes_read = stats.compaction_bytes_read;
  res.compaction_bytes_written = stats.compaction_bytes_written;
  res.compaction_cpu_nanos = stats.compaction_cpu_nanos;
  res.compaction_io_ratio =
      stats.foreground_bytes_written > 0
          ? static_cast<double>(stats.compaction_bytes_read +
                                stats.compaction_bytes_written) /
                static_cast<double>(stats.foreground_bytes_written)
          : 0.0;
  res.write_amplification =
      stats.foreground_bytes_written > 0
          ? static_cast<double>(stats.foreground_bytes_written +
                                stats.compaction_bytes_written) /
                static_cast<double>(stats.foreground_bytes_written)
          : 1.0;

  res.total_lookups = stats.total_lookups;
  res.sstable_files_touched = stats.sstable_files_touched;
  res.read_amplification =
      stats.total_lookups > 0
          ? static_cast<double>(stats.sstable_files_touched) /
                static_cast<double>(stats.total_lookups)
          : 0.0;
  res.num_active_levels = stats.num_active_levels;
  res.write_stall_count = stats.write_stall_count;
  return res;
}

void RunReadAmplificationStudy(const std::string& out_csv,
                               const std::string& base_dir) {
  std::ofstream out(out_csv, std::ios::trunc);
  out << "level_count,avg_files_touched_per_lookup\n";

  std::string db_dir = base_dir + "/bench_read_amp";
  std::filesystem::remove_all(db_dir);

  Options opt;
  opt.memtable_type = MemTableType::LOCK_FREE;
  opt.memtable_max_size = 64 * 1024;
  opt.l0_compaction_trigger = 2;
  opt.l1_max_bytes = 64 * 1024;
  opt.level_size_multiplier = 2;

  std::unique_ptr<DB> db;
  DB::Open(db_dir, opt, &db);

  for (int batch = 1; batch <= 5; ++batch) {
    for (int i = 0; i < 1500; ++i) {
      std::string k = FormatFixedKey(static_cast<uint64_t>(batch * 2000 + i));
      std::string v(100, 'x');
      db->Put(k, v);
    }
    db->ForceFlush();
    if (batch >= 2) {
      db->WaitForCompaction();
    }

    DBStats before = db->GetStats();
    for (int q = 0; q < 500; ++q) {
      std::string k = FormatFixedKey(static_cast<uint64_t>(q * 15));
      std::string val;
      db->Get(k, &val);
    }
    DBStats after = db->GetStats();
    uint64_t lookups = after.total_lookups - before.total_lookups;
    uint64_t touched = after.sstable_files_touched - before.sstable_files_touched;
    double ra = lookups > 0 ? static_cast<double>(touched) / lookups : 0.0;
    out << batch << "," << std::fixed << std::setprecision(4) << ra << "\n";
  }

  db->Close();
  std::filesystem::remove_all(db_dir);
}

}  // namespace
}  // namespace lsm

int main(int argc, char** argv) {
  using namespace lsm;
  std::string results_dir = "results";
  std::size_t trials = 5;
  std::size_t total_ops_per_run = 24000;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--results-dir" && i + 1 < argc) {
      results_dir = argv[++i];
    } else if (arg == "--trials" && i + 1 < argc) {
      trials = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (arg == "--ops" && i + 1 < argc) {
      total_ops_per_run = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
  }

  std::filesystem::create_directories(results_dir);

  // 1. Generate Slide 3 Zipfian vs Uniform Rank Probability CSV & Table
  auto slide3 = WorkloadGenerator::MeasureRankProbabilities(1000, 1.0, 2000000, 5, 42);
  std::cout << WorkloadGenerator::FormatSlide3Table(
      slide3, results_dir + "/slide3_distribution.csv");

  // 2. Full Matrix: {LOCK_FREE, SHARDED, GLOBAL_LOCK} x {UNIFORM, ZIPFIAN} x {1,2,4,8,16,32}
  std::string matrix_csv = results_dir + "/benchmark_matrix.csv";
  MetricsReporter::WriteCsvHeader(matrix_csv);

  const std::vector<MemTableType> memtables = {
      MemTableType::LOCK_FREE, MemTableType::SHARDED,
      MemTableType::GLOBAL_LOCK};
  const std::vector<KeyDistribution> dists = {KeyDistribution::UNIFORM,
                                              KeyDistribution::ZIPFIAN};
  const std::vector<std::size_t> thread_counts = {1, 2, 4, 8, 16, 32};

  for (KeyDistribution dist : dists) {
    for (MemTableType mt : memtables) {
      for (std::size_t th : thread_counts) {
        std::size_t ops_per_th = std::max<std::size_t>(500, total_ops_per_run / th);
        for (std::size_t trial = 1; trial <= trials; ++trial) {
          BenchmarkRunResult r =
              RunSingleCell(mt, dist, th, ops_per_th, trial, results_dir);
          MetricsReporter::AppendCsvRow(matrix_csv, r);
          if (trial == 1) {
            std::cout << "[" << MetricsReporter::DistributionName(dist) << " | "
                      << std::setw(11) << MetricsReporter::MemTableTypeName(mt)
                      << " | threads=" << std::setw(2) << th
                      << "] throughput=" << std::fixed << std::setprecision(0)
                      << r.throughput_ops_per_sec
                      << " ops/s, p99=" << std::setprecision(2)
                      << r.p99_write_latency_us << " us\n";
          }
        }
      }
    }
  }

  // 3. Read Amplification study
  RunReadAmplificationStudy(results_dir + "/read_amplification.csv", results_dir);
  std::cout << "Benchmarks completed. Raw CSVs written to " << results_dir << "/\n";
  return 0;
}
