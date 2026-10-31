#include "sstable/sstable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "util/crc32c.h"

namespace lsm {

class SSTableIterator final : public InternalIterator {
 public:
  explicit SSTableIterator(const SSTableReader* reader)
      : reader_(reader), block_idx_(0), entry_idx_(0), valid_(false) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    if (reader_->index_.empty()) {
      valid_ = false;
      return;
    }
    LoadBlock(0);
    entry_idx_ = 0;
    valid_ = !current_entries_.empty();
  }

  void Seek(const InternalKey& target) override {
    if (reader_->index_.empty()) {
      valid_ = false;
      return;
    }
    auto it = std::lower_bound(
        reader_->index_.begin(), reader_->index_.end(), target,
        [](const SSTableReader::IndexEntry& ie, const InternalKey& k) {
          return ie.max_key.Compare(k) < 0;
        });
    if (it == reader_->index_.end()) {
      valid_ = false;
      return;
    }
    std::size_t bidx =
        static_cast<std::size_t>(std::distance(reader_->index_.begin(), it));
    LoadBlock(bidx);
    for (std::size_t i = 0; i < current_entries_.size(); ++i) {
      if (current_entries_[i].key.Compare(target) >= 0) {
        entry_idx_ = i;
        valid_ = true;
        return;
      }
    }
    if (bidx + 1 < reader_->index_.size()) {
      LoadBlock(bidx + 1);
      entry_idx_ = 0;
      valid_ = !current_entries_.empty();
    } else {
      valid_ = false;
    }
  }

  void Next() override {
    if (!valid_) return;
    ++entry_idx_;
    if (entry_idx_ < current_entries_.size()) return;
    if (block_idx_ + 1 < reader_->index_.size()) {
      LoadBlock(block_idx_ + 1);
      entry_idx_ = 0;
      valid_ = !current_entries_.empty();
    } else {
      valid_ = false;
    }
  }

  InternalKey key() const override { return current_entries_[entry_idx_].key; }

  std::string_view value() const override {
    return current_entries_[entry_idx_].value;
  }

 private:
  void LoadBlock(std::size_t bidx) {
    block_idx_ = bidx;
    current_entries_.clear();
    const auto& ie = reader_->index_[bidx];
    reader_->ReadBlock(ie.offset, ie.size, &current_entries_);
  }

  const SSTableReader* reader_;
  std::size_t block_idx_;
  std::size_t entry_idx_;
  bool valid_;
  std::vector<BlockEntry> current_entries_;
};

SSTableReader::~SSTableReader() {
  if (fd_ >= 0) ::close(fd_);
}

Status SSTableReader::Open(const std::string& filepath,
                           std::shared_ptr<SSTableReader>* reader_out) {
  int fd = ::open(filepath.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("Failed to open SSTable: " + filepath);
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 52) {
    ::close(fd);
    return Status::Corruption("Invalid SSTable size: " + filepath);
  }

  char footer[52];
  if (::pread(fd, footer, 52, st.st_size - 52) != 52) {
    ::close(fd);
    return Status::IOError("Failed to read SSTable footer");
  }

  uint64_t magic = 0;
  std::memcpy(&magic, footer + 44, 8);
  if (magic != kSSTableMagicNumber) {
    ::close(fd);
    return Status::Corruption("Bad SSTable magic number");
  }

  uint32_t stored_fcrc = 0;
  std::memcpy(&stored_fcrc, footer + 40, 4);
  if (stored_fcrc != crc32c::Mask(crc32c::Value(footer, 40))) {
    ::close(fd);
    return Status::Corruption("Bad SSTable footer checksum");
  }

  uint64_t filter_offset = 0, filter_size = 0;
  uint64_t index_offset = 0, index_size = 0, num_entries = 0;
  std::memcpy(&filter_offset, footer + 0, 8);
  std::memcpy(&filter_size, footer + 8, 8);
  std::memcpy(&index_offset, footer + 16, 8);
  std::memcpy(&index_size, footer + 24, 8);
  std::memcpy(&num_entries, footer + 32, 8);

  std::shared_ptr<SSTableReader> r(new SSTableReader());
  r->filepath_ = filepath;
  r->fd_ = fd;
  r->file_size_ = static_cast<uint64_t>(st.st_size);
  r->num_entries_ = num_entries;

  r->bloom_filter_.resize(filter_size);
  if (::pread(fd, r->bloom_filter_.data(), filter_size,
              static_cast<off_t>(filter_offset)) !=
      static_cast<ssize_t>(filter_size)) {
    return Status::IOError("Failed to read Bloom filter block");
  }

  std::vector<char> idx_buf(index_size);
  if (::pread(fd, idx_buf.data(), index_size,
              static_cast<off_t>(index_offset)) !=
      static_cast<ssize_t>(index_size) ||
      index_size < 8) {
    return Status::IOError("Failed to read Index block");
  }

  uint32_t stored_icrc = 0;
  std::memcpy(&stored_icrc, idx_buf.data() + index_size - 4, 4);
  if (stored_icrc != crc32c::Mask(crc32c::Value(idx_buf.data(), index_size - 4))) {
    return Status::Corruption("Index block checksum mismatch");
  }

  uint32_t num_idx = 0;
  std::memcpy(&num_idx, idx_buf.data(), 4);
  r->index_.reserve(num_idx);
  std::size_t off = 4;
  for (uint32_t i = 0; i < num_idx; ++i) {
    if (off + 41 > index_size - 4) {
      return Status::Corruption("Truncated index block");
    }
    IndexEntry ie;
    std::memcpy(ie.max_key.user_key, idx_buf.data() + off, kDefaultKeySize);
    off += kDefaultKeySize;
    std::memcpy(&ie.max_key.seq, idx_buf.data() + off, 8);
    off += 8;
    uint8_t t = 0;
    std::memcpy(&t, idx_buf.data() + off, 1);
    off += 1;
    ie.max_key.type =
        (t == static_cast<uint8_t>(ValueType::kDelete)) ? ValueType::kDelete
                                                        : ValueType::kPut;
    std::memcpy(&ie.offset, idx_buf.data() + off, 8);
    off += 8;
    std::memcpy(&ie.size, idx_buf.data() + off, 8);
    off += 8;
    r->index_.push_back(ie);
  }

  *reader_out = std::move(r);
  return Status::OK();
}

Status SSTableReader::ReadBlock(uint64_t offset, uint64_t size,
                                std::vector<BlockEntry>* entries) const {
  std::string buf(size, '\0');
  if (::pread(fd_, buf.data(), size, static_cast<off_t>(offset)) !=
      static_cast<ssize_t>(size)) {
    return Status::IOError("Failed to read SSTable data block");
  }
  return BlockReader::Parse(buf, entries);
}

Status SSTableReader::Get(std::string_view user_key, bool* found,
                          std::string* value, ValueType* type,
                          uint64_t* seq) const {
  *found = false;
  // Section 3.5: Bloom filter must skip files on negative lookups!
  if (!BloomFilter::KeyMayMatch(user_key, bloom_filter_)) {
    return Status::OK();
  }

  InternalKey target(user_key, UINT64_MAX, ValueType::kPut);
  auto it = std::lower_bound(
      index_.begin(), index_.end(), target,
      [](const IndexEntry& ie, const InternalKey& k) {
        return ie.max_key.Compare(k) < 0;
      });
  if (it == index_.end()) {
    return Status::OK();
  }

  std::vector<BlockEntry> entries;
  Status s = ReadBlock(it->offset, it->size, &entries);
  if (!s.ok()) return s;

  for (const auto& entry : entries) {
    int cmp = std::memcmp(entry.key.user_key, user_key.data(), kDefaultKeySize);
    if (cmp == 0) {
      *found = true;
      *type = entry.key.type;
      *seq = entry.key.seq;
      if (entry.key.type == ValueType::kPut && value != nullptr) {
        *value = entry.value;
      }
      return Status::OK();
    }
    if (cmp > 0) break;
  }
  return Status::OK();
}

std::unique_ptr<InternalIterator> SSTableReader::NewIterator() const {
  return std::make_unique<SSTableIterator>(this);
}

}  // namespace lsm
