#include "db/db_impl.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lsm {

Status DBImpl::RecoverFromDisk() {
  // 1. Rebuild the latest immutable Version from MANIFEST (Section 3.6)
  Status s = versions_.Recover();
  if (!s.ok()) return s;

  // 2. Discover any remaining `.wal` segments in `db_path_` and replay them in
  // ascending segment order (Section 3.4).
  DIR* dir = ::opendir(db_path_.c_str());
  if (dir == nullptr) return Status::OK();

  std::vector<uint64_t> wal_segments;
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(dir)) != nullptr) {
    std::string name(ent->d_name);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".wal") {
      uint64_t seg_id = std::strtoull(name.c_str(), nullptr, 10);
      if (seg_id > 0) {
        wal_segments.push_back(seg_id);
      }
    }
  }
  ::closedir(dir);

  std::sort(wal_segments.begin(), wal_segments.end());
  uint64_t max_seq = versions_.LastSequence();

  for (uint64_t seg_id : wal_segments) {
    if (seg_id >= next_wal_segment_id_.load(std::memory_order_relaxed)) {
      next_wal_segment_id_.store(seg_id + 1, std::memory_order_relaxed);
    }

    std::string wal_path = versions_.WalFilePath(seg_id);
    WalReader reader(wal_path);
    MemTable* recovered_mem = MemTable::Create(
        options_.memtable_type, options_.sharded_memtable_shards);
    std::size_t replayed = 0;
    s = reader.ReplayInto(recovered_mem, &max_seq, &replayed);
    if (s.ok() && replayed > 0) {
      versions_.SetLastSequence(max_seq);
      FlushMemTableToL0(recovered_mem, seg_id);
    } else {
      ::unlink(wal_path.c_str());
    }
    recovered_mem->Unref();
  }

  versions_.SetLastSequence(max_seq);
  return Status::OK();
}

}  // namespace lsm
