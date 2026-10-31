#include "sstable/sstable.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include "util/crc32c.h"

namespace lsm {

namespace {

Status WriteAll(int fd, const char* data, std::size_t len, uint64_t* offset) {
  std::size_t written = 0;
  while (written < len) {
    ssize_t n = ::pwrite(fd, data + written, len - written,
                         static_cast<off_t>(*offset + written));
    if (n <= 0) return Status::IOError("SSTable pwrite failed");
    written += static_cast<std::size_t>(n);
  }
  *offset += len;
  return Status::OK();
}

}  // namespace

SSTableBuilder::SSTableBuilder(const std::string& filepath,
                               uint64_t file_number, const Options& options)
    : filepath_(filepath),
      file_number_(file_number),
      options_(options),
      fd_(-1),
      offset_(0),
      num_entries_(0),
      max_seq_(0) {
  fd_ = ::open(filepath_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
}

SSTableBuilder::~SSTableBuilder() {
  if (fd_ >= 0) ::close(fd_);
}

Status SSTableBuilder::Add(const InternalKey& key, std::string_view value) {
  if (fd_ < 0) return Status::IOError("SSTable file not open: " + filepath_);

  if (num_entries_ == 0) {
    smallest_ = key;
  }
  largest_ = key;
  current_block_max_key_ = key;
  if (key.seq > max_seq_) max_seq_ = key.seq;

  std::string ukey(key.user_key, kDefaultKeySize);
  if (user_keys_for_bloom_.empty() || user_keys_for_bloom_.back() != ukey) {
    user_keys_for_bloom_.push_back(std::move(ukey));
  }

  data_block_.Add(key, value);
  ++num_entries_;

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    return FlushCurrentDataBlock();
  }
  return Status::OK();
}

Status SSTableBuilder::FlushCurrentDataBlock() {
  if (data_block_.empty()) return Status::OK();
  uint64_t blk_offset = offset_;
  std::string raw = data_block_.Finish();
  Status s = WriteAll(fd_, raw.data(), raw.size(), &offset_);
  if (!s.ok()) return s;

  IndexEntry ie;
  ie.max_key = current_block_max_key_;
  ie.offset = blk_offset;
  ie.size = raw.size();
  index_entries_.push_back(ie);
  data_block_.Reset();
  return Status::OK();
}

Status SSTableBuilder::Finish(SSTableMeta* meta_out) {
  if (fd_ < 0) return Status::IOError("SSTable file not open");
  Status s = FlushCurrentDataBlock();
  if (!s.ok()) return s;

  // 1. Write Bloom filter block
  std::string bloom_bytes;
  BloomFilter::CreateFilter(user_keys_for_bloom_, options_.bloom_bits_per_key,
                            &bloom_bytes);
  uint64_t filter_offset = offset_;
  uint64_t filter_size = bloom_bytes.size();
  s = WriteAll(fd_, bloom_bytes.data(), bloom_bytes.size(), &offset_);
  if (!s.ok()) return s;

  // 2. Write Index block
  std::string index_buf;
  uint32_t num_idx = static_cast<uint32_t>(index_entries_.size());
  index_buf.append(reinterpret_cast<const char*>(&num_idx), 4);
  for (const auto& ie : index_entries_) {
    index_buf.append(ie.max_key.user_key, kDefaultKeySize);
    index_buf.append(reinterpret_cast<const char*>(&ie.max_key.seq), 8);
    uint8_t t = static_cast<uint8_t>(ie.max_key.type);
    index_buf.append(reinterpret_cast<const char*>(&t), 1);
    index_buf.append(reinterpret_cast<const char*>(&ie.offset), 8);
    index_buf.append(reinterpret_cast<const char*>(&ie.size), 8);
  }
  uint32_t idx_crc =
      crc32c::Mask(crc32c::Value(index_buf.data(), index_buf.size()));
  index_buf.append(reinterpret_cast<const char*>(&idx_crc), 4);

  uint64_t index_offset = offset_;
  uint64_t index_size = index_buf.size();
  s = WriteAll(fd_, index_buf.data(), index_buf.size(), &offset_);
  if (!s.ok()) return s;

  // 3. Write 52-byte Footer:
  // [filter_offset:8][filter_size:8][index_offset:8][index_size:8][num_entries:8][footer_crc:4][magic:8]
  char footer[52];
  std::memcpy(footer + 0, &filter_offset, 8);
  std::memcpy(footer + 8, &filter_size, 8);
  std::memcpy(footer + 16, &index_offset, 8);
  std::memcpy(footer + 24, &index_size, 8);
  std::memcpy(footer + 32, &num_entries_, 8);
  uint32_t footer_crc = crc32c::Mask(crc32c::Value(footer, 40));
  std::memcpy(footer + 40, &footer_crc, 4);
  uint64_t magic = kSSTableMagicNumber;
  std::memcpy(footer + 44, &magic, 8);

  s = WriteAll(fd_, footer, sizeof(footer), &offset_);
  if (!s.ok()) return s;

  ::fdatasync(fd_);
  ::close(fd_);
  fd_ = -1;

  if (meta_out != nullptr) {
    meta_out->file_number = file_number_;
    meta_out->file_size = offset_;
    meta_out->num_entries = num_entries_;
    meta_out->smallest = smallest_;
    meta_out->largest = largest_;
    meta_out->max_seq = max_seq_;
  }
  return Status::OK();
}

}  // namespace lsm
