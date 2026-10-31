#include "sstable/sstable.h"

#include <cstring>
#include "util/crc32c.h"

namespace lsm {

void BlockBuilder::Add(const InternalKey& key, std::string_view value) {
  if (count_ == 0) {
    buffer_.resize(4, '\0');  // Reserve 4B header for entry count
  }
  buffer_.append(key.user_key, kDefaultKeySize);
  buffer_.append(reinterpret_cast<const char*>(&key.seq), 8);
  uint8_t t = static_cast<uint8_t>(key.type);
  buffer_.append(reinterpret_cast<const char*>(&t), 1);

  uint32_t vlen = static_cast<uint32_t>(value.size());
  buffer_.append(reinterpret_cast<const char*>(&vlen), 4);
  if (vlen > 0) {
    buffer_.append(value.data(), vlen);
  }
  ++count_;
}

std::string BlockBuilder::Finish() {
  if (count_ == 0) {
    buffer_.resize(4, '\0');
  }
  std::memcpy(buffer_.data(), &count_, 4);
  uint32_t crc = crc32c::Mask(crc32c::Value(buffer_.data(), buffer_.size()));
  buffer_.append(reinterpret_cast<const char*>(&crc), 4);
  return buffer_;
}

void BlockBuilder::Reset() {
  buffer_.clear();
  count_ = 0;
}

Status BlockReader::Parse(std::string_view raw_block,
                          std::vector<BlockEntry>* entries_out) {
  if (raw_block.size() < 8) {
    return Status::Corruption("Block too small");
  }
  const std::size_t payload_size = raw_block.size() - 4;
  uint32_t stored_crc = 0;
  std::memcpy(&stored_crc, raw_block.data() + payload_size, 4);
  uint32_t actual_crc =
      crc32c::Mask(crc32c::Value(raw_block.data(), payload_size));
  if (stored_crc != actual_crc) {
    return Status::Corruption("Block checksum mismatch");
  }

  uint32_t count = 0;
  std::memcpy(&count, raw_block.data(), 4);
  entries_out->clear();
  entries_out->reserve(count);

  std::size_t offset = 4;
  for (uint32_t i = 0; i < count; ++i) {
    if (offset + 29 > payload_size) {
      return Status::Corruption("Truncated block entry header");
    }
    BlockEntry entry;
    std::memcpy(entry.key.user_key, raw_block.data() + offset, kDefaultKeySize);
    offset += kDefaultKeySize;
    std::memcpy(&entry.key.seq, raw_block.data() + offset, 8);
    offset += 8;
    uint8_t t = 0;
    std::memcpy(&t, raw_block.data() + offset, 1);
    offset += 1;
    entry.key.type =
        (t == static_cast<uint8_t>(ValueType::kDelete)) ? ValueType::kDelete
                                                        : ValueType::kPut;

    uint32_t vlen = 0;
    std::memcpy(&vlen, raw_block.data() + offset, 4);
    offset += 4;
    if (offset + vlen > payload_size) {
      return Status::Corruption("Truncated block entry value");
    }
    entry.value.assign(raw_block.data() + offset, vlen);
    offset += vlen;
    entries_out->push_back(std::move(entry));
  }
  return Status::OK();
}

}  // namespace lsm
