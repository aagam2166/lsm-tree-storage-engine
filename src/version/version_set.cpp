#include "version/version.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace lsm {

VersionSet::VersionSet(const std::string& db_path, const Options& options)
    : db_path_(db_path),
      options_(options),
      manifest_(db_path),
      current_(nullptr),
      next_file_number_(1),
      last_sequence_(0),
      min_log_number_(0) {
  Version* initial = new Version(options_.max_levels, this);
  live_versions_.push_back(initial);
  // Relaxed store in constructor before VersionSet is shared across threads.
  current_.store(initial, std::memory_order_relaxed);
}

VersionSet::~VersionSet() {
  std::lock_guard<std::recursive_mutex> lock(install_mu_);
  Version* cur = current_.load(std::memory_order_acquire);
  if (cur != nullptr) {
    current_.store(nullptr, std::memory_order_relaxed);
    cur->Unref();
  }
}

std::string VersionSet::SSTableFilePath(uint64_t file_number) const {
  std::ostringstream oss;
  oss << db_path_ << "/" << std::setw(6) << std::setfill('0') << file_number
      << ".sst";
  return oss.str();
}

std::string VersionSet::WalFilePath(uint64_t segment_id) const {
  std::ostringstream oss;
  oss << db_path_ << "/" << std::setw(6) << std::setfill('0') << segment_id
      << ".wal";
  return oss.str();
}

Version* VersionSet::AcquireCurrentVersion() noexcept {
  std::lock_guard<std::recursive_mutex> lock(install_mu_);
  // Acquire load pairs with the release CAS in LogAndApply.
  Version* v = current_.load(std::memory_order_acquire);
  v->Ref();
  return v;
}

Status VersionSet::Recover() {
  std::vector<VersionEdit> edits;
  Status s = manifest_.OpenOrRecover(&edits);
  if (!s.ok()) return s;

  for (const auto& edit : edits) {
    Version* base = current_.load(std::memory_order_acquire);
    Version* next_v = new Version(options_.max_levels, this);
    s = ApplyEditToBuilder(base, edit, next_v, /*open_readers=*/false);
    if (!s.ok()) {
      delete next_v;
      return s;
    }
    if (edit.has_next_file_number) {
      uint64_t cur_nf = next_file_number_.load(std::memory_order_relaxed);
      if (edit.next_file_number > cur_nf) {
        next_file_number_.store(edit.next_file_number,
                                std::memory_order_relaxed);
      }
    }
    if (edit.has_last_sequence) {
      SetLastSequence(edit.last_sequence);
    }
    if (edit.has_min_log_number) {
      min_log_number_.store(edit.min_log_number, std::memory_order_relaxed);
    }
    live_versions_.push_back(next_v);
    current_.store(next_v, std::memory_order_release);
    base->Unref();
  }

  // Open readers exclusively for surviving live SSTables in the final version
  Version* cur = current_.load(std::memory_order_acquire);
  all_known_sstables_.clear();
  if (cur != nullptr) {
    for (std::size_t lvl = 0; lvl < cur->NumLevels(); ++lvl) {
      for (auto& meta : cur->levels_[lvl]) {
        if (!meta.reader) {
          s = SSTableReader::Open(SSTableFilePath(meta.file_number),
                                  &meta.reader);
          if (!s.ok()) return s;
        }
        all_known_sstables_.insert(meta.file_number);
      }
    }
  }

  return Status::OK();
}

Status VersionSet::ApplyEditToBuilder(const Version* base,
                                      const VersionEdit& edit, Version* dst,
                                      bool open_readers) {
  for (std::size_t lvl = 0; lvl < options_.max_levels; ++lvl) {
    dst->levels_[lvl] = base->levels_[lvl];
  }

  for (const auto& del : edit.deleted_files) {
    std::size_t lvl = static_cast<std::size_t>(del.first);
    if (lvl >= dst->levels_.size()) continue;
    auto& files = dst->levels_[lvl];
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const FileMetaData& f) {
                                 return f.file_number == del.second;
                               }),
                files.end());
  }

  for (const auto& add : edit.added_files) {
    std::size_t lvl = static_cast<std::size_t>(add.first);
    if (lvl >= dst->levels_.size()) continue;
    FileMetaData meta = add.second;
    if (open_readers && !meta.reader) {
      Status s = SSTableReader::Open(SSTableFilePath(meta.file_number),
                                     &meta.reader);
      if (!s.ok()) return s;
    }
    if (meta.file_number >= next_file_number_.load(std::memory_order_relaxed)) {
      next_file_number_.store(meta.file_number + 1, std::memory_order_relaxed);
    }
    if (open_readers) {
      all_known_sstables_.insert(meta.file_number);
    }
    dst->levels_[lvl].push_back(std::move(meta));
  }

  // Sort L0 by file_number ascending; sort L1..Ln by smallest key ascending.
  if (!dst->levels_.empty()) {
    std::sort(dst->levels_[0].begin(), dst->levels_[0].end(),
              [](const FileMetaData& a, const FileMetaData& b) {
                return a.file_number < b.file_number;
              });
  }
  for (std::size_t lvl = 1; lvl < dst->levels_.size(); ++lvl) {
    std::sort(dst->levels_[lvl].begin(), dst->levels_[lvl].end(),
              [](const FileMetaData& a, const FileMetaData& b) {
                return a.smallest.Compare(b.smallest) < 0;
              });
  }
  return Status::OK();
}

Status VersionSet::LogAndApply(const VersionEdit& raw_edit) {
  std::lock_guard<std::recursive_mutex> lock(install_mu_);
  VersionEdit edit = raw_edit;
  edit.has_next_file_number = true;
  edit.next_file_number = next_file_number_.load(std::memory_order_relaxed);
  edit.has_last_sequence = true;
  edit.last_sequence = last_sequence_.load(std::memory_order_acquire);

  // Acquire load of current_ to build the next immutable Version snapshot.
  Version* base = current_.load(std::memory_order_acquire);
  Version* next_v = new Version(options_.max_levels, this);
  Status s = ApplyEditToBuilder(base, edit, next_v);
  if (!s.ok()) {
    delete next_v;
    return s;
  }

  s = manifest_.AppendEdit(edit);
  if (!s.ok()) {
    delete next_v;
    return s;
  }

  live_versions_.push_back(next_v);
  // Section 3.6: Atomically swap the current-version pointer via CAS.
  // Release on success publishes `next_v` to concurrent readers.
  Version* expected = base;
  current_.compare_exchange_strong(expected, next_v, std::memory_order_acq_rel,
                                   std::memory_order_acquire);
  base->Unref();
  CleanupObsoleteFiles();
  return Status::OK();
}

void VersionSet::OnVersionDestroyed(const Version* v) {
  std::lock_guard<std::recursive_mutex> lock(install_mu_);
  live_versions_.erase(
      std::remove(live_versions_.begin(), live_versions_.end(), v),
      live_versions_.end());
}

void VersionSet::CleanupObsoleteFiles() {
  std::set<uint64_t> referenced;
  for (const Version* v : live_versions_) {
    for (std::size_t lvl = 0; lvl < v->NumLevels(); ++lvl) {
      for (const auto& f : v->LevelFiles(lvl)) {
        referenced.insert(f.file_number);
      }
    }
  }

  std::vector<uint64_t> obsolete;
  for (uint64_t fnum : all_known_sstables_) {
    if (referenced.find(fnum) == referenced.end()) {
      obsolete.push_back(fnum);
    }
  }
  for (uint64_t fnum : obsolete) {
    all_known_sstables_.erase(fnum);
    ::unlink(SSTableFilePath(fnum).c_str());
  }
}

}  // namespace lsm
