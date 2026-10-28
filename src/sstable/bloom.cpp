#include "sstable/sstable.h"

#include <algorithm>
#include "util/crc32c.h"

namespace lsm {

namespace {

inline uint32_t BloomHash(std::string_view key) noexcept {
  return crc32c::Value(key.data(), key.size());
}

}  // namespace

void BloomFilter::CreateFilter(const std::vector<std::string>& keys,
                               std::size_t bits_per_key, std::string* dst) {
  std::size_t bits = keys.size() * bits_per_key;
  if (bits < 64) bits = 64;
  std::size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  std::size_t k = static_cast<std::size_t>(bits_per_key * 0.69);
  if (k < 1) k = 1;
  if (k > 30) k = 30;

  dst->assign(bytes + 1, '\0');
  (*dst)[bytes] = static_cast<char>(k);
  char* array = dst->data();

  for (const auto& key : keys) {
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (std::size_t j = 0; j < k; ++j) {
      const uint32_t bitpos = h % static_cast<uint32_t>(bits);
      array[bitpos / 8] |= static_cast<char>(1U << (bitpos % 8));
      h += delta;
    }
  }
}

bool BloomFilter::KeyMayMatch(std::string_view key, std::string_view filter) {
  if (filter.size() < 2) return false;
  const std::size_t bytes = filter.size() - 1;
  const std::size_t bits = bytes * 8;
  const std::size_t k = static_cast<uint8_t>(filter[bytes]);
  if (k > 30) return true;

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (std::size_t j = 0; j < k; ++j) {
    const uint32_t bitpos = h % static_cast<uint32_t>(bits);
    if ((static_cast<uint8_t>(filter[bitpos / 8]) & (1U << (bitpos % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace lsm
