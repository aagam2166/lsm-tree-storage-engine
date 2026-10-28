#ifndef LSM_SRC_UTIL_CRC32C_H_
#define LSM_SRC_UTIL_CRC32C_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm {
namespace crc32c {

uint32_t Value(const char* data, std::size_t length);
uint32_t Value(std::string_view s);
uint32_t Mask(uint32_t crc);
uint32_t Unmask(uint32_t masked_crc);

}  // namespace crc32c
}  // namespace lsm

#endif  // LSM_SRC_UTIL_CRC32C_H_
