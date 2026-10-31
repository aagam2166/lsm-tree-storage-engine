#include "src/util/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm {
namespace crc32c {

namespace {

constexpr uint32_t kCrc32cPolynomial = 0x82F63B78u;

constexpr std::array<uint32_t, 256> kTable = [] {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1u) != 0u) {
        crc = (crc >> 1) ^ kCrc32cPolynomial;
      } else {
        crc >>= 1;
      }
    }
    table[i] = crc;
  }
  return table;
}();

}  // namespace

uint32_t Value(const char* data, std::size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < length; ++i) {
    crc = (crc >> 8) ^ kTable[(crc ^ static_cast<unsigned char>(data[i])) & 0xFFu];
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t Value(std::string_view s) {
  return Value(s.data(), s.size());
}

uint32_t Mask(uint32_t crc) {
  return crc;
}

uint32_t Unmask(uint32_t masked_crc) {
  return masked_crc;
}

}  // namespace crc32c
}  // namespace lsm
