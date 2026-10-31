#ifndef LSM_INCLUDE_LSM_TYPES_H_
#define LSM_INCLUDE_LSM_TYPES_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "lsm/status.h"

namespace lsm {

constexpr std::size_t kDefaultKeySize = 16;

enum class ValueType : uint8_t {
  kPut = 0,
  kDelete = 1,
};

enum class MemTableType : uint8_t {
  LOCK_FREE = 0,
  GLOBAL_LOCK = 1,
  SHARDED = 2,
};

enum class SyncPolicy : uint8_t {
  NO_SYNC = 0,
  SYNC_EVERY_WRITE = 1,
  GROUP_COMMIT = 2,
};

enum class KeyDistribution : uint8_t {
  UNIFORM = 0,
  ZIPFIAN = 1,
};

struct InternalKey {
  char user_key[kDefaultKeySize] = {};
  uint64_t seq = 0;
  ValueType type = ValueType::kPut;

  InternalKey() = default;
  InternalKey(std::string_view user_key_view, uint64_t seq_in,
              ValueType t = ValueType::kPut) : seq(seq_in), type(t) {
    std::memset(user_key, 0, kDefaultKeySize);
    const std::size_t n = std::min<std::size_t>(user_key_view.size(), kDefaultKeySize);
    std::memcpy(user_key, user_key_view.data(), n);
  }

  int Compare(const InternalKey& other) const noexcept {
    const int cmp = std::memcmp(user_key, other.user_key, kDefaultKeySize);
    if (cmp != 0) return cmp;
    if (seq != other.seq) return (seq > other.seq) ? -1 : 1;
    return (type == other.type) ? 0 : (type > other.type ? -1 : 1);
  }

  std::string UserKeyString() const {
    return std::string(user_key, std::find(user_key, user_key + kDefaultKeySize,
                                          '\0') - user_key);
  }

  std::string_view UserKeyView() const noexcept {
    return std::string_view(user_key, kDefaultKeySize);
  }
};

inline std::string FormatFixedKey(uint64_t val) {
  char buf[kDefaultKeySize + 1];
  std::memset(buf, 0, sizeof(buf));
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(val));
  return std::string(buf, kDefaultKeySize);
}

struct Options {
  MemTableType memtable_type = MemTableType::LOCK_FREE;
  std::size_t sharded_memtable_shards = 16;
  std::size_t memtable_max_size = 64 * 1024;
  std::size_t max_immutable_memtables = 4;
  SyncPolicy sync_policy = SyncPolicy::NO_SYNC;
  uint64_t group_commit_interval_us = 1000;
  std::size_t block_size = 4 * 1024;
  std::size_t bloom_bits_per_key = 10;
  std::size_t max_levels = 8;
  std::size_t l0_compaction_trigger = 4;
  uint64_t l1_max_bytes = 256 * 1024;
  std::size_t level_size_multiplier = 10;
  std::size_t compaction_threads = 2;
};

}  // namespace lsm

#endif  // LSM_INCLUDE_LSM_TYPES_H_
