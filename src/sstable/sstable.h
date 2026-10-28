#ifndef LSM_SRC_SSTABLE_SSTABLE_H_
#define LSM_SRC_SSTABLE_SSTABLE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/iterator.h"
#include "lsm/options.h"
#include "lsm/status.h"
#include "lsm/types.h"

namespace lsm {

constexpr uint64_t kSSTableMagicNumber = 0x4C534D5452454531ULL;  // "LSMTREE1"

class BloomFilter {
 public:
  static void CreateFilter(const std::vector<std::string>& keys,
                           std::size_t bits_per_key, std::string* dst);
  static bool KeyMayMatch(std::string_view key, std::string_view filter);
};

struct BlockEntry {
  InternalKey key;
  std::string value;
};

class BlockBuilder {
 public:
  BlockBuilder() = default;
  void Add(const InternalKey& key, std::string_view value);
  std::string Finish();
  void Reset();
  std::size_t CurrentSizeEstimate() const noexcept { return buffer_.size(); }
  bool empty() const noexcept { return count_ == 0; }

 private:
  std::string buffer_;
  uint32_t count_ = 0;
};

class BlockReader {
 public:
  static Status Parse(std::string_view raw_block,
                      std::vector<BlockEntry>* entries_out);
};

struct SSTableMeta {
  uint64_t file_number = 0;
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  InternalKey smallest;
  InternalKey largest;
  uint64_t max_seq = 0;
};

class SSTableBuilder {
 public:
  SSTableBuilder(const std::string& filepath, uint64_t file_number,
                 const Options& options);
  ~SSTableBuilder();

  Status Add(const InternalKey& key, std::string_view value);
  Status Finish(SSTableMeta* meta_out);

 private:
  Status FlushCurrentDataBlock();

  struct IndexEntry {
    InternalKey max_key;
    uint64_t offset;
    uint64_t size;
  };

  std::string filepath_;
  uint64_t file_number_;
  Options options_;
  int fd_;
  uint64_t offset_;
  uint64_t num_entries_;
  InternalKey smallest_;
  InternalKey largest_;
  uint64_t max_seq_;
  BlockBuilder data_block_;
  InternalKey current_block_max_key_;
  std::vector<IndexEntry> index_entries_;
  std::vector<std::string> user_keys_for_bloom_;
};

class SSTableReader {
 public:
  static Status Open(const std::string& filepath,
                     std::shared_ptr<SSTableReader>* reader_out);
  ~SSTableReader();

  // Point lookup using Bloom filter + Index block binary search.
  // Sets `*found = true` if user_key is present in this SSTable.
  Status Get(std::string_view user_key, bool* found, std::string* value,
             ValueType* type, uint64_t* seq) const;

  std::unique_ptr<InternalIterator> NewIterator() const;

  uint64_t NumEntries() const noexcept { return num_entries_; }
  const std::string& filepath() const noexcept { return filepath_; }

 private:
  friend class SSTableIterator;
  SSTableReader() : fd_(-1), file_size_(0), num_entries_(0) {}

  struct IndexEntry {
    InternalKey max_key;
    uint64_t offset;
    uint64_t size;
  };

  Status ReadBlock(uint64_t offset, uint64_t size,
                   std::vector<BlockEntry>* entries) const;

  std::string filepath_;
  int fd_;
  uint64_t file_size_;
  uint64_t num_entries_;
  std::string bloom_filter_;
  std::vector<IndexEntry> index_;
};

}  // namespace lsm

#endif  // LSM_SRC_SSTABLE_SSTABLE_H_
