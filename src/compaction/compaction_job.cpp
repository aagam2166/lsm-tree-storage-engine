#include "compaction/compaction.h"

#include <unistd.h>

#include <cstring>
#include <utility>

#include "util/clock.h"

namespace lsm {

CompactionJob::CompactionJob(CompactionTask task, VersionSet* vset,
                             const Options& options,
                             DisjointRangeRegistry* registry,
                             CompactionMetrics* metrics)
    : task_(std::move(task)),
      vset_(vset),
      options_(options),
      registry_(registry),
      metrics_(metrics) {}

CompactionJob::~CompactionJob() {
  if (task_.range_slot >= 0 && registry_ != nullptr) {
    registry_->ReleaseRange(task_.range_slot);
    task_.range_slot = -1;
  }
  if (task_.base_version != nullptr) {
    task_.base_version->Unref();
    task_.base_version = nullptr;
  }
}

Status CompactionJob::Run() {
  const uint64_t cpu_start = Clock::ThreadCpuNanos();

  // Verify that all input files are still live in the current Version before
  // doing heavy merge work.
  uint64_t bytes_in = 0;
  std::vector<std::unique_ptr<InternalIterator>> iters;
  for (const auto& f : task_.level_inputs) {
    bytes_in += f.file_size;
    if (f.reader) {
      auto it = f.reader->NewIterator();
      it->SeekToFirst();
      iters.push_back(std::move(it));
    }
  }
  for (const auto& f : task_.output_level_inputs) {
    bytes_in += f.file_size;
    if (f.reader) {
      auto it = f.reader->NewIterator();
      it->SeekToFirst();
      iters.push_back(std::move(it));
    }
  }

  // Check if there are any deeper levels below `output_level` that could hold
  // older versions of keys. If no deeper levels are non-empty, we can safely
  // drop tombstones (`ValueType::kDelete`) during compaction!
  bool bottommost_level = true;
  if (task_.base_version != nullptr) {
    for (std::size_t lvl = static_cast<std::size_t>(task_.output_level + 1);
         lvl < task_.base_version->NumLevels(); ++lvl) {
      if (!task_.base_version->LevelFiles(lvl).empty()) {
        bottommost_level = false;
        break;
      }
    }
  }

  uint64_t out_file_num = vset_->AllocateFileNumber();
  std::string out_path = vset_->SSTableFilePath(out_file_num);
  SSTableBuilder builder(out_path, out_file_num, options_);

  bool have_last_user_key = false;
  char last_user_key[kDefaultKeySize];
  std::memset(last_user_key, 0, kDefaultKeySize);
  uint64_t entries_written = 0;

  // Multi-way merge across all input SSTables (newest version wins because
  // InternalKey orders higher sequence numbers first!).
  while (true) {
    int smallest_idx = -1;
    for (std::size_t i = 0; i < iters.size(); ++i) {
      if (!iters[i]->Valid()) continue;
      if (smallest_idx < 0 ||
          iters[i]->key().Compare(
              iters[static_cast<std::size_t>(smallest_idx)]->key()) < 0) {
        smallest_idx = static_cast<int>(i);
      }
    }
    if (smallest_idx < 0) break;

    InternalKey ikey = iters[static_cast<std::size_t>(smallest_idx)]->key();
    std::string val(iters[static_cast<std::size_t>(smallest_idx)]->value());
    iters[static_cast<std::size_t>(smallest_idx)]->Next();

    bool is_First_for_user_key =
        !have_last_user_key ||
        std::memcmp(ikey.user_key, last_user_key, kDefaultKeySize) != 0;

    if (is_First_for_user_key) {
      std::memcpy(last_user_key, ikey.user_key, kDefaultKeySize);
      have_last_user_key = true;

      // Drop tombstones when compacting into the bottommost active level.
      if (ikey.type == ValueType::kDelete && bottommost_level) {
        continue;
      }

      Status s = builder.Add(ikey, val);
      if (!s.ok()) return s;
      ++entries_written;
    }
    // Else: older version of `last_user_key` — shadowed by newer version, drop!
  }

  SSTableMeta out_meta{};
  if (entries_written > 0) {
    Status s = builder.Finish(&out_meta);
    if (!s.ok()) return s;
  } else {
    ::unlink(out_path.c_str());
  }

  // Verify input files are still present in the latest Version before applying
  Version* latest = vset_->AcquireCurrentVersion();
  bool inputs_still_live = true;
  for (const auto& in_f : task_.level_inputs) {
    bool found = false;
    for (const auto& cur_f :
         latest->LevelFiles(static_cast<std::size_t>(task_.input_level))) {
      if (cur_f.file_number == in_f.file_number) {
        found = true;
        break;
      }
    }
    if (!found) {
      inputs_still_live = false;
      break;
    }
  }
  latest->Unref();

  if (!inputs_still_live) {
    if (entries_written > 0) {
      ::unlink(out_path.c_str());
    }
    return Status::OK();
  }

  VersionEdit edit;
  for (const auto& f : task_.level_inputs) {
    edit.deleted_files.emplace_back(task_.input_level, f.file_number);
  }
  for (const auto& f : task_.output_level_inputs) {
    edit.deleted_files.emplace_back(task_.output_level, f.file_number);
  }
  if (entries_written > 0) {
    FileMetaData meta;
    meta.file_number = out_meta.file_number;
    meta.file_size = out_meta.file_size;
    meta.num_entries = out_meta.num_entries;
    meta.smallest = out_meta.smallest;
    meta.largest = out_meta.largest;
    meta.max_seq = out_meta.max_seq;
    edit.added_files.emplace_back(task_.output_level, std::move(meta));
  }

  Status s = vset_->LogAndApply(edit);
  const uint64_t cpu_elapsed = Clock::ThreadCpuNanos() - cpu_start;

  if (metrics_ != nullptr) {
    // Relaxed fetch_add for background compaction telemetry counters.
    metrics_->bytes_read.fetch_add(bytes_in, std::memory_order_relaxed);
    metrics_->bytes_written.fetch_add(out_meta.file_size,
                                      std::memory_order_relaxed);
    metrics_->cpu_nanos.fetch_add(cpu_elapsed, std::memory_order_relaxed);
    metrics_->jobs_completed.fetch_add(1, std::memory_order_relaxed);
  }
  return s;
}

}  // namespace lsm
