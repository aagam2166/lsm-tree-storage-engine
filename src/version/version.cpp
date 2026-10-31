#include "version/version.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace lsm {

Version::Version(std::size_t max_levels, VersionSet* vset)
    : refs_(1), vset_(vset), levels_(max_levels) {}

Version::~Version() = default;

void Version::Unref() noexcept {
  // Acquire-release fetch_sub ensures all reads through this Version complete
  // before the Version is destroyed and its obsolete SSTables are unlinked.
  int32_t prev = refs_.fetch_sub(1, std::memory_order_acq_rel);
  assert(prev >= 1);
  if (prev == 1) {
    if (vset_ != nullptr) {
      vset_->OnVersionDestroyed(this);
    }
    delete this;
  }
}

std::size_t Version::ActiveLevelCount() const noexcept {
  std::size_t count = 0;
  for (const auto& lvl : levels_) {
    if (!lvl.empty()) ++count;
  }
  return count;
}

uint64_t Version::LevelBytes(std::size_t level) const noexcept {
  if (level >= levels_.size()) return 0;
  uint64_t sum = 0;
  for (const auto& f : levels_[level]) {
    sum += f.file_size;
  }
  return sum;
}

Status Version::Get(std::string_view user_key, bool* found, std::string* value,
                    ValueType* type, uint64_t* seq,
                    uint64_t* files_touched) const {
  *found = false;

  // Level 0: files may overlap. Search newest file first (highest file_number).
  if (!levels_.empty() && !levels_[0].empty()) {
    std::vector<const FileMetaData*> l0_candidates;
    for (const auto& f : levels_[0]) {
      if (std::memcmp(user_key.data(), f.smallest.user_key, kDefaultKeySize) >= 0 &&
          std::memcmp(user_key.data(), f.largest.user_key, kDefaultKeySize) <= 0) {
        l0_candidates.push_back(&f);
      }
    }
    std::sort(l0_candidates.begin(), l0_candidates.end(),
              [](const FileMetaData* a, const FileMetaData* b) {
                return a->file_number > b->file_number;
              });

    for (const FileMetaData* f : l0_candidates) {
      if (!f->reader) continue;
      if (files_touched != nullptr) (*files_touched)++;
      bool hit = false;
      Status s = f->reader->Get(user_key, &hit, value, type, seq);
      if (!s.ok()) return s;
      if (hit) {
        *found = true;
        return Status::OK();
      }
    }
  }

  // Levels 1..n: files are non-overlapping and sorted by key range.
  for (std::size_t level = 1; level < levels_.size(); ++level) {
    const auto& files = levels_[level];
    if (files.empty()) continue;

    auto it = std::lower_bound(
        files.begin(), files.end(), user_key,
        [](const FileMetaData& f, std::string_view k) {
          return std::memcmp(f.largest.user_key, k.data(), kDefaultKeySize) < 0;
        });
    if (it != files.end() &&
        std::memcmp(user_key.data(), it->smallest.user_key, kDefaultKeySize) >= 0) {
      if (it->reader) {
        if (files_touched != nullptr) (*files_touched)++;
        bool hit = false;
        Status s = it->reader->Get(user_key, &hit, value, type, seq);
        if (!s.ok()) return s;
        if (hit) {
          *found = true;
          return Status::OK();
        }
      }
    }
  }

  return Status::OK();
}

}  // namespace lsm
