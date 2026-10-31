#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "lsm/db.h"
#include "lsm/options.h"

namespace {

void PrintBanner() {
  std::cout << "\n======================================================\n";
  std::cout << "  Lock-Free LSM-Tree Storage Engine Interactive CLI   \n";
  std::cout << "======================================================\n";
  std::cout << "Available Commands:\n";
  std::cout << "  PUT <key> <val>     : Insert or update a key-value pair\n";
  std::cout << "  GET <key>           : Retrieve value for a key\n";
  std::cout << "  DEL <key>           : Delete a key (writes a tombstone)\n";
  std::cout << "  SCAN <key> [limit]  : Scan range of keys in sorted order (default 10)\n";
  std::cout << "  BULK_PUT <pfx> <N>  : High-speed ingestion simulation of N records\n";
  std::cout << "  FLUSH               : Flush active MemTable to Level 0 SSTable\n";
  std::cout << "  COMPACT             : Trigger background compaction\n";
  std::cout << "  STATS               : Display live engine metrics\n";
  std::cout << "  HELP                : Show this command menu\n";
  std::cout << "  QUIT / EXIT         : Close database and exit\n";
  std::cout << "------------------------------------------------------\n\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_dir = "./demo_db";
  if (argc > 1) {
    db_dir = argv[1];
  }

  lsm::Options opt;
  opt.memtable_type = lsm::MemTableType::LOCK_FREE;
  opt.memtable_max_size = 64 * 1024;
  opt.sync_policy = lsm::SyncPolicy::SYNC_EVERY_WRITE;
  opt.compaction_threads = 2;

  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(db_dir, opt, &db);
  if (!s.ok()) {
    std::cerr << "[ERROR] Could not open database at '" << db_dir
              << "': " << s.ToString() << "\n";
    return 1;
  }

  PrintBanner();
  std::cout << "Opened database directory: " << db_dir << "\n\n";

  std::string line;
  while (true) {
    std::cout << "lsm> ";
    if (!std::getline(std::cin, line)) {
      break;
    }

    std::istringstream iss(line);
    std::string cmd;
    if (!(iss >> cmd)) {
      continue;
    }

    for (char& c : cmd) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    if (cmd == "QUIT" || cmd == "EXIT" || cmd == "Q") {
      std::cout << "Closing database cleanly...\n";
      break;
    } else if (cmd == "HELP") {
      PrintBanner();
    } else if (cmd == "PUT") {
      std::string key, val;
      if (iss >> key) {
        std::string rest;
        std::getline(iss, rest);
        auto pos = rest.find_first_not_of(" \t");
        if (pos != std::string::npos) {
          val = rest.substr(pos);
        }
        lsm::Status st = db->Put(key, val);
        if (st.ok()) {
          std::cout << "[OK] Stored: '" << key << "' => '" << val << "'\n";
        } else {
          std::cout << "[ERROR] " << st.ToString() << "\n";
        }
      } else {
        std::cout << "Usage: PUT <key> <value>\n";
      }
    } else if (cmd == "GET") {
      std::string key;
      if (iss >> key) {
        std::string val;
        lsm::Status st = db->Get(key, &val);
        if (st.ok()) {
          std::cout << "[FOUND] " << key << " = '" << val << "'\n";
        } else if (st.IsNotFound()) {
          std::cout << "[NOT FOUND] Key '" << key << "' does not exist.\n";
        } else {
          std::cout << "[ERROR] " << st.ToString() << "\n";
        }
      } else {
        std::cout << "Usage: GET <key>\n";
      }
    } else if (cmd == "DEL" || cmd == "DELETE") {
      std::string key;
      if (iss >> key) {
        lsm::Status st = db->Delete(key);
        if (st.ok()) {
          std::cout << "[OK] Deleted: '" << key << "' (Tombstone written)\n";
        } else {
          std::cout << "[ERROR] " << st.ToString() << "\n";
        }
      } else {
        std::cout << "Usage: DEL <key>\n";
      }
    } else if (cmd == "SCAN") {
      std::string start_key;
      std::size_t limit = 10;
      if (iss >> start_key) {
        std::size_t user_limit = 0;
        if (iss >> user_limit && user_limit > 0) {
          limit = user_limit;
        }
        std::vector<std::pair<std::string, std::string>> results;
        lsm::Status st = db->Scan(start_key, limit, &results);
        if (st.ok()) {
          std::cout << "[SCAN] Retrieved " << results.size() << " records starting from '"
                    << start_key << "':\n";
          for (std::size_t i = 0; i < results.size(); ++i) {
            std::cout << "  [" << (i + 1) << "] " << results[i].first
                      << " => '" << results[i].second << "'\n";
          }
          if (results.empty()) {
            std::cout << "  (no keys found >= '" << start_key << "')\n";
          }
        } else {
          std::cout << "[ERROR] " << st.ToString() << "\n";
        }
      } else {
        std::cout << "Usage: SCAN <start_key> [limit=10]\n";
      }
    } else if (cmd == "BULK_PUT") {
      std::string prefix;
      std::size_t count = 0;
      if (iss >> prefix >> count) {
        if (count == 0 || count > 100000) {
          std::cout << "Count must be between 1 and 100,000\n";
          continue;
        }
        std::cout << "Simulating high-velocity IoT ingestion of " << count
                  << " records with prefix '" << prefix << "'...\n";
        auto t0 = std::chrono::high_resolution_clock::now();
        for (std::size_t i = 1; i <= count; ++i) {
          std::ostringstream koss;
          koss << prefix << "_" << std::setw(6) << std::setfill('0') << i;
          std::string k = koss.str();
          std::string v = "temp=" + std::to_string(20 + (i % 15)) +
                          ".5C,humidity=" + std::to_string(45 + (i % 20)) +
                          "%,v=" + std::to_string(i);
          lsm::Status st = db->Put(k, v);
          if (!st.ok()) {
            std::cout << "[ERROR at record " << i << "] " << st.ToString() << "\n";
            break;
          }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        double ops_sec = (count / (elapsed_ms / 1000.0));
        std::cout << "[OK] Ingested " << count << " records in " << std::fixed
                  << std::setprecision(2) << elapsed_ms << " ms ("
                  << std::setprecision(0) << ops_sec << " writes/sec)!\n";
      } else {
        std::cout << "Usage: BULK_PUT <prefix> <count>\n";
      }
    } else if (cmd == "FLUSH") {
      std::cout << "Flushing active MemTable to Level 0 SSTable...\n";
      lsm::Status st = db->ForceFlush();
      if (st.ok()) {
        std::cout << "[OK] Flush completed.\n";
      } else {
        std::cout << "[ERROR] " << st.ToString() << "\n";
      }
    } else if (cmd == "COMPACT") {
      std::cout << "Triggering background compaction and waiting...\n";
      lsm::Status st = db->WaitForCompaction();
      if (st.ok()) {
        std::cout << "[OK] Compaction completed.\n";
      } else {
        std::cout << "[ERROR] " << st.ToString() << "\n";
      }
    } else if (cmd == "STATS") {
      lsm::DBStats stats = db->GetStats();
      std::cout << "\n--- Engine Telemetry & Statistics ---\n";
      std::cout << "  Foreground Bytes Written : " << stats.foreground_bytes_written << " bytes\n";
      std::cout << "  Total Point Lookups      : " << stats.total_lookups << "\n";
      std::cout << "  SSTable Files Touched    : " << stats.sstable_files_touched << "\n";
      std::cout << "  Active On-Disk Levels    : " << stats.num_active_levels << "\n";
      std::cout << "  Compaction Bytes Read    : " << stats.compaction_bytes_read << "\n";
      std::cout << "  Compaction Bytes Written : " << stats.compaction_bytes_written << "\n";
      std::cout << "  Compaction CPU Time      : " << (stats.compaction_cpu_nanos / 1000000.0) << " ms\n";
      std::cout << "  Write Stalls             : " << stats.write_stall_count << "\n";
      std::cout << "-------------------------------------\n\n";
    } else {
      std::cout << "Unknown command: '" << cmd << "'. Type HELP for menu.\n";
    }
  }

  db->Close();
  std::cout << "Database closed. Exiting.\n";
  return 0;
}
