#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lsm/db.h"
#include "lsm/types.h"

// Child process used by the crash-recovery test harness (Section 7.2).
// Writes continuously with SYNC_EVERY_WRITE or GROUP_COMMIT and sends each
// acknowledged key_id over a pipe (`ack_fd`) to the parent process until the
// parent sends SIGKILL at a random time (including mid-flush / mid-compaction).
int main(int argc, char** argv) {
  if (argc < 4) return 1;
  std::string db_path = argv[1];
  int ack_fd = std::atoi(argv[2]);
  int policy_int = std::atoi(argv[3]);

  lsm::Options opt;
  opt.memtable_type = lsm::MemTableType::LOCK_FREE;
  opt.memtable_max_size = 32 * 1024;  // Frequent flushes & compactions
  opt.l0_compaction_trigger = 2;
  opt.sync_policy = (policy_int == 2) ? lsm::SyncPolicy::GROUP_COMMIT
                                      : lsm::SyncPolicy::SYNC_EVERY_WRITE;

  std::unique_ptr<lsm::DB> db;
  if (!lsm::DB::Open(db_path, opt, &db).ok()) return 2;

  for (uint64_t key_id = 1; key_id <= 100000; ++key_id) {
    std::string k = lsm::FormatFixedKey(key_id);
    std::string v(100, static_cast<char>('A' + (key_id % 26)));
    lsm::Status s = db->Put(k, v);
    if (s.ok()) {
      // Notify parent pipe that `key_id` was durably acknowledged!
      ssize_t n = ::write(ack_fd, &key_id, sizeof(key_id));
      (void)n;
    }
  }
  return 0;
}
