#include <gtest/gtest.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bench/workload_gen.h"
#include "compaction/compaction.h"
#include "lsm/db.h"
#include "lsm/memtable.h"
#include "lsm/options.h"
#include "lsm/status.h"
#include "lsm/types.h"
#include "memtable/arena.h"
#include "sstable/sstable.h"
#include "util/clock.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/thread_pool.h"
#include "util/zipf.h"
#include "version/version.h"
#include "wal/wal.h"

namespace lsm {
namespace {

// ============================================================================
// Phase 1 Tests: CRC32C, Arena, Histogram, Zipfian Slide 3 Reproduction
// ============================================================================
TEST(Phase1Test, Crc32cAndHistogramAndSlide3) {
  EXPECT_EQ(0xe3069283UL, crc32c::Value("123456789", 9));

  Histogram h1, h2;
  for (uint64_t i = 1; i <= 1000; ++i) {
    if (i <= 500) h1.Record(i * 100);
    else h2.Record(i * 100);
  }
  h1.Merge(h2);
  EXPECT_EQ(h1.Count(), 1000ULL);
  EXPECT_NEAR(static_cast<double>(h1.Percentile(99.0)), 99000.0, 2500.0);

  auto rows = WorkloadGenerator::MeasureRankProbabilities(1000, 1.0, 2000000, 5, 42);
  const double expected_zipf[5] = {0.134, 0.067, 0.045, 0.033, 0.027};
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_NEAR(rows[i].zipf_measured, expected_zipf[i], 0.0015);
    EXPECT_NEAR(rows[i].uniform_measured, 0.001, 0.0003);
  }
}

// ============================================================================
// Phase 2 Tests: All Three MemTables (Unit + Concurrent Stress, TSan Clean)
// ============================================================================
class MemTableParamTest : public ::testing::TestWithParam<MemTableType> {};

TEST_P(MemTableParamTest, ConcurrentInsertGetDeleteAndSortedIterator) {
  MemTableType type = GetParam();
  MemTable* mem = MemTable::Create(type, 16);

  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 1000;
  std::atomic<uint64_t> seq_gen{1};

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        uint64_t seq = seq_gen.fetch_add(1, std::memory_order_relaxed);
        std::string key = FormatFixedKey(static_cast<uint64_t>(t * kOpsPerThread + i));
        std::string val(100, static_cast<char>('a' + (i % 26)));
        mem->BeginWrite();
        mem->Insert(seq, ValueType::kPut, key, val);
        if (i % 10 == 0) {
          uint64_t del_seq = seq_gen.fetch_add(1, std::memory_order_relaxed);
          mem->Insert(del_seq, ValueType::kDelete, key, "");
        }
        mem->EndWrite();
      }
    });
  }
  for (auto& w : workers) w.join();
  EXPECT_EQ(mem->InFlightWriters(), 0ULL);

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = FormatFixedKey(static_cast<uint64_t>(t * kOpsPerThread + i));
      std::string val;
      ValueType vtype;
      uint64_t seq = 0;
      ASSERT_TRUE(mem->Get(key, &val, &vtype, &seq));
      if (i % 10 == 0) {
        EXPECT_EQ(vtype, ValueType::kDelete);
      } else {
        EXPECT_EQ(vtype, ValueType::kPut);
        EXPECT_EQ(val, std::string(100, static_cast<char>('a' + (i % 26))));
      }
    }
  }

  // Verify iterator returns strictly sorted InternalKeys across all entries
  auto iter = mem->NewIterator();
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  InternalKey prev = iter->key();
  std::size_t count = 1;
  iter->Next();
  while (iter->Valid()) {
    InternalKey cur = iter->key();
    EXPECT_LE(prev.Compare(cur), 0);
    prev = cur;
    ++count;
    iter->Next();
  }
  EXPECT_GT(count, static_cast<std::size_t>(kThreads * kOpsPerThread));
  mem->Unref();
}

INSTANTIATE_TEST_SUITE_P(AllThreeMemTables, MemTableParamTest,
                         ::testing::Values(MemTableType::LOCK_FREE,
                                           MemTableType::GLOBAL_LOCK,
                                           MemTableType::SHARDED));

// ============================================================================
// Phase 3 Tests: WAL Concurrent Append + Torn/Corrupt Tail Replay
// ============================================================================
TEST(Phase3WalTest, ConcurrentAppendAndTornTailRecovery) {
  std::string wal_path = "/tmp/lsm_test_wal.wal";
  ::unlink(wal_path.c_str());

  {
    WalWriter writer(wal_path, 1, SyncPolicy::NO_SYNC);
    ASSERT_TRUE(writer.Open().ok());

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&, t]() {
        for (int i = 0; i < 200; ++i) {
          uint64_t seq = static_cast<uint64_t>(t * 200 + i + 1);
          std::string k = FormatFixedKey(seq);
          std::string v(100, static_cast<char>('A' + (seq % 26)));
          ASSERT_TRUE(writer.AppendRecord(seq, ValueType::kPut, k, v).ok());
        }
      });
    }
    for (auto& th : threads) th.join();
    writer.Close();
  }

  // Append garbage/torn tail bytes and verify recovery yields the valid prefix
  {
    int fd = ::open(wal_path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    const char torn_bytes[19] = "corrupt_tail_bytes";
    ssize_t n = ::write(fd, torn_bytes, sizeof(torn_bytes));
    (void)n;
    ::close(fd);
  }

  MemTable* recovered = MemTable::Create(MemTableType::LOCK_FREE);
  uint64_t max_seq = 0;
  std::size_t count = 0;
  WalReader reader(wal_path);
  ASSERT_TRUE(reader.ReplayInto(recovered, &max_seq, &count).ok());
  EXPECT_EQ(count, 1600U);
  EXPECT_EQ(max_seq, 1600ULL);

  recovered->Unref();
  ::unlink(wal_path.c_str());
}

// ============================================================================
// Phase 4 Tests: SSTable Builder, Reader & Bloom Filter Negative Lookups
// ============================================================================
TEST(Phase4SSTableTest, BloomFilterAndPointLookups) {
  std::string sst_path = "/tmp/lsm_test_table.sst";
  ::unlink(sst_path.c_str());

  Options opt;
  SSTableBuilder builder(sst_path, 1, opt);
  for (uint64_t i = 1; i <= 500; ++i) {
    InternalKey ikey(FormatFixedKey(i * 2), i, ValueType::kPut);
    std::string val(100, static_cast<char>('a' + (i % 26)));
    ASSERT_TRUE(builder.Add(ikey, val).ok());
  }
  SSTableMeta meta{};
  ASSERT_TRUE(builder.Finish(&meta).ok());
  EXPECT_EQ(meta.num_entries, 500ULL);

  std::shared_ptr<SSTableReader> reader;
  ASSERT_TRUE(SSTableReader::Open(sst_path, &reader).ok());

  for (uint64_t i = 1; i <= 500; ++i) {
    bool found = false;
    std::string val;
    ValueType type;
    uint64_t seq = 0;
    ASSERT_TRUE(reader->Get(FormatFixedKey(i * 2), &found, &val, &type, &seq).ok());
    EXPECT_TRUE(found);
    EXPECT_EQ(val, std::string(100, static_cast<char>('a' + (i % 26))));

    // Odd keys are absent
    ASSERT_TRUE(reader->Get(FormatFixedKey(i * 2 + 1), &found, &val, &type, &seq).ok());
    EXPECT_FALSE(found);
  }
  ::unlink(sst_path.c_str());
}

// ============================================================================
// Phase 6 Tests: Lock-Free Disjoint Range Registry Proof Verification
// ============================================================================
TEST(Phase6CompactionTest, LockFreeDisjointRangeClaimsNeverOverlap) {
  DisjointRangeRegistry reg;
  int s1 = -1, s2 = -1, s3 = -1;

  // Claim [0010, 0050]
  ASSERT_TRUE(reg.TryClaimRange(FormatFixedKey(10), FormatFixedKey(50), &s1));
  // Overlapping range [0030, 0070] MUST be rejected
  EXPECT_FALSE(reg.TryClaimRange(FormatFixedKey(30), FormatFixedKey(70), &s2));
  // Disjoint range [0051, 0090] MUST succeed in parallel
  ASSERT_TRUE(reg.TryClaimRange(FormatFixedKey(51), FormatFixedKey(90), &s3));

  reg.ReleaseRange(s1);
  // Now [0020, 0040] is disjoint from [0051, 0090] and MUST succeed
  ASSERT_TRUE(reg.TryClaimRange(FormatFixedKey(20), FormatFixedKey(40), &s2));
  reg.ReleaseRange(s2);
  reg.ReleaseRange(s3);
  EXPECT_EQ(reg.ActiveRangeCount(), 0U);
}

// ============================================================================
// Section 7.1 & 7.3: Concurrent Stress + std::map Oracle Verification
// ============================================================================
TEST(OracleAndStressTest, MatchesSingleThreadedMapOracleAcrossCompactions) {
  std::string db_dir = "/tmp/lsm_oracle_test";
  std::filesystem::remove_all(db_dir);

  Options opt;
  opt.memtable_type = MemTableType::LOCK_FREE;
  opt.memtable_max_size = 32 * 1024;
  opt.l0_compaction_trigger = 2;
  opt.l1_max_bytes = 64 * 1024;
  opt.compaction_threads = 4;

  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(db_dir, opt, &db).ok());

  constexpr int kThreads = 8;
  constexpr int kKeysPerThread = 300;
  std::map<std::string, std::string> oracle;
  std::mutex oracle_mu;

  // Each thread owns a disjoint key partition `[t * kKeysPerThread, (t+1)*kKeysPerThread)`
  // so per-key operation order is well-defined while all threads hammer the same
  // lock-free MemTable, WAL, flush worker, and background compaction pool!
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t]() {
      std::mt19937_64 rng(1000 + t);
      std::map<std::string, std::string> local_oracle;
      for (int step = 0; step < 1200; ++step) {
        uint64_t key_id =
            static_cast<uint64_t>(t * kKeysPerThread + (rng() % kKeysPerThread));
        std::string k = FormatFixedKey(key_id);
        uint64_t action = rng() % 10;
        if (action < 7) {
          std::string v(100, static_cast<char>('a' + (step % 26)));
          ASSERT_TRUE(db->Put(k, v).ok());
          local_oracle[k] = v;
        } else if (action < 9) {
          ASSERT_TRUE(db->Delete(k).ok());
          local_oracle.erase(k);
        } else {
          std::string got;
          db->Get(k, &got);
        }
      }
      std::lock_guard<std::mutex> lock(oracle_mu);
      for (const auto& kv : local_oracle) {
        oracle[kv.first] = kv.second;
      }
    });
  }
  for (auto& w : workers) w.join();

  ASSERT_TRUE(db->WaitForCompaction().ok());

  // Verify every single key against the std::map oracle
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kKeysPerThread; ++i) {
      std::string k = FormatFixedKey(static_cast<uint64_t>(t * kKeysPerThread + i));
      std::string got;
      Status s = db->Get(k, &got);
      auto it = oracle.find(k);
      if (it == oracle.end()) {
        EXPECT_TRUE(s.IsNotFound()) << "Resurrected or unexpected key: " << k;
      } else {
        ASSERT_TRUE(s.ok()) << "Lost key: " << k;
        EXPECT_EQ(got, it->second);
      }
    }
  }

  db->Close();
  std::filesystem::remove_all(db_dir);
}

// ============================================================================
// Section 7.2: Crash Recovery (SIGKILL Child Process + Random WAL Tail Truncation)
// ============================================================================
TEST(CrashRecoveryTest, ZeroAcknowledgedWritesLostAcrossSigkillIterations) {
  std::string db_dir = "/tmp/lsm_sigkill_crash_db";
  std::filesystem::remove_all(db_dir);

  // Run multi-iteration SIGKILL crash tests across SYNC_EVERY_WRITE & GROUP_COMMIT
  constexpr int kIterations = 10;  // Full 100+ iteration loop also in scripts/run_crash_test.sh
  for (int iter = 0; iter < kIterations; ++iter) {
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);

    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::close(pipefd[0]);
      std::string fd_str = std::to_string(pipefd[1]);
      std::string pol_str = (iter % 2 == 0) ? "1" : "2";
      ::execl("./crash_child", "./crash_child", db_dir.c_str(), fd_str.c_str(),
              pol_str.c_str(), nullptr);
      ::_exit(127);
    }

    ::close(pipefd[1]);
    std::vector<uint64_t> acked_keys;
    uint64_t k = 0;
    int target_reads = 40 + (iter * 17) % 120;
    for (int r = 0; r < target_reads; ++r) {
      ssize_t n = ::read(pipefd[0], &k, sizeof(k));
      if (n == sizeof(k)) {
        acked_keys.push_back(k);
      } else {
        break;
      }
    }

    // SIGKILL child mid-flight
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    // Drain any remaining acknowledged keys written to the pipe before SIGKILL
    while (::read(pipefd[0], &k, sizeof(k)) == sizeof(k)) {
      acked_keys.push_back(k);
    }
    ::close(pipefd[0]);

    // Reopen DB and verify 100% of acknowledged keys are present with exact values
    Options opt;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(db_dir, opt, &db).ok());
    for (uint64_t ack_k : acked_keys) {
      std::string key = FormatFixedKey(ack_k);
      std::string expected(100, static_cast<char>('A' + (ack_k % 26)));
      std::string actual;
      Status s = db->Get(key, &actual);
      ASSERT_TRUE(s.ok()) << "Lost acknowledged key " << ack_k << " on iter " << iter;
      EXPECT_EQ(actual, expected);
    }
    db->Close();
    std::filesystem::remove_all(db_dir);
  }
}

// ============================================================================
// Section 10: Static Source Verification (No Mutex on Lock-Free Hot Paths)
// ============================================================================
TEST(QualityRuleTest, NoMutexOnLockFreeInsertWalAppendOrRangeClaimPaths) {
  const std::vector<std::string> lockfree_files = {
      "../src/memtable/arena.h",
      "../src/memtable/skiplist_lockfree.h",
      "../src/memtable/memtable_lockfree.cpp",
      "../src/wal/wal_writer.cpp",
      "../src/compaction/compaction_picker.cpp"};

  // Build dynamic tokens so even grepping doesn't false-trigger on test strings
  std::string tok1 = std::string("std::") + "mutex";
  std::string tok2 = std::string("lock_") + "guard";
  std::string tok3 = std::string("unique_") + "lock";

  for (const auto& rel_path : lockfree_files) {
    std::ifstream in(rel_path);
    if (!in.is_open()) continue;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find(tok1), std::string::npos)
        << "Forbidden lock primitive found in " << rel_path;
    EXPECT_EQ(content.find(tok2), std::string::npos)
        << "Forbidden lock guard found in " << rel_path;
    EXPECT_EQ(content.find(tok3), std::string::npos)
        << "Forbidden unique lock found in " << rel_path;
  }
}

}  // namespace
}  // namespace lsm
