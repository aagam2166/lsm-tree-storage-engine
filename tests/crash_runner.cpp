#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.h"
#include "lsm/memtable.h"
#include "lsm/options.h"
#include "lsm/status.h"
#include "lsm/types.h"
#include "wal/wal.h"

using namespace lsm;

static void TestWalTornTail() {
  std::cout << "[TEST 1/2] WAL Concurrent Append & Torn-Tail Recovery Test...\n";
  std::string wal_path = "/tmp/lsm_test_wal.wal";
  ::unlink(wal_path.c_str());

  {
    WalWriter writer(wal_path, 1, SyncPolicy::NO_SYNC);
    if (!writer.Open().ok()) {
      std::cerr << "FAIL: Could not open WAL\n";
      std::exit(1);
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&, t]() {
        for (int i = 0; i < 200; ++i) {
          uint64_t seq = static_cast<uint64_t>(t * 200 + i + 1);
          std::string k = FormatFixedKey(seq);
          std::string v(100, static_cast<char>('A' + (seq % 26)));
          assert(writer.AppendRecord(seq, ValueType::kPut, k, v).ok());
        }
      });
    }
    for (auto& th : threads) th.join();
    writer.Close();
  }

  // Append garbage/torn tail bytes to simulate sudden mid-write crash
  {
    int fd = ::open(wal_path.c_str(), O_WRONLY | O_APPEND);
    assert(fd >= 0);
    const char torn_bytes[19] = "corrupt_tail_bytes";
    ssize_t n = ::write(fd, torn_bytes, sizeof(torn_bytes));
    (void)n;
    ::close(fd);
  }

  MemTable* recovered = MemTable::Create(MemTableType::LOCK_FREE);
  uint64_t max_seq = 0;
  std::size_t count = 0;
  WalReader reader(wal_path);
  Status s = reader.ReplayInto(recovered, &max_seq, &count);
  assert(s.ok());
  assert(count == 1600);
  assert(max_seq == 1600);

  recovered->Unref();
  ::unlink(wal_path.c_str());
  std::cout << "  -> PASS: 1600/1600 valid records recovered, torn tail safely discarded via CRC32C validation.\n";
}

static void TestSigkillRecovery(const std::string& child_bin) {
  std::cout << "[TEST 2/2] Running 10 SIGKILL Crash-Recovery Cycles on Live Engine...\n";
  std::string db_dir = "/tmp/lsm_sigkill_crash_db";

  for (int iter = 0; iter < 10; ++iter) {
    std::filesystem::remove_all(db_dir);
    int pipefd[2];
    assert(::pipe(pipefd) == 0);

    pid_t pid = ::fork();
    assert(pid >= 0);
    if (pid == 0) {
      ::close(pipefd[0]);
      std::string fd_str = std::to_string(pipefd[1]);
      std::string pol_str = (iter % 2 == 0) ? "1" : "2";
      ::execl(child_bin.c_str(), child_bin.c_str(), db_dir.c_str(), fd_str.c_str(),
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

    // SIGKILL child process unannounced while writing
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    // Drain any remaining acknowledged keys written to pipe before SIGKILL
    while (::read(pipefd[0], &k, sizeof(k)) == sizeof(k)) {
      acked_keys.push_back(k);
    }
    ::close(pipefd[0]);

    // Reopen database and verify 100% of durably acknowledged keys are present
    Options opt;
    std::unique_ptr<DB> db;
    Status open_st = DB::Open(db_dir, opt, &db);
    assert(open_st.ok());

    for (uint64_t ack_k : acked_keys) {
      std::string key = FormatFixedKey(ack_k);
      std::string expected(100, static_cast<char>('A' + (ack_k % 26)));
      std::string actual;
      Status get_st = db->Get(key, &actual);
      if (!get_st.ok() || actual != expected) {
        std::cerr << "FAIL: Lost acknowledged key " << ack_k << " on cycle " << iter << "\n";
        std::exit(1);
      }
    }
    db->Close();
    std::cout << "  -> Cycle " << (iter + 1) << "/10 passed: "
              << acked_keys.size() << " acknowledged keys verified intact after SIGKILL.\n";
    std::filesystem::remove_all(db_dir);
  }
}

int main(int argc, char** argv) {
  std::string child_bin = "./crash_child";
  if (argc > 1) {
    child_bin = argv[1];
  }
  std::cout << "=======================================================\n";
  std::cout << "     LSM-Tree Crash Recovery & Durability Test Suite   \n";
  std::cout << "=======================================================\n";
  TestWalTornTail();
  TestSigkillRecovery(child_bin);
  std::cout << "\n[ALL CRASH TESTS PASSED] Zero acknowledged writes lost under SIGKILL.\n";
  return 0;
}
