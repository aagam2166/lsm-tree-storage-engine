#include "wal/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "util/crc32c.h"

namespace lsm {

WalReader::WalReader(const std::string& path) : path_(path) {}

Status WalReader::ReplayInto(MemTable* memtable, uint64_t* max_seq,
                             std::size_t* records_replayed) {
  int fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("Cannot open WAL for replay: " + path_);
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IOError("fstat failed on WAL: " + path_);
  }

  const std::size_t file_size = static_cast<std::size_t>(st.st_size);
  std::vector<char> data(file_size);
  std::size_t total_read = 0;
  while (total_read < file_size) {
    ssize_t n = ::pread(fd, data.data() + total_read, file_size - total_read,
                        static_cast<off_t>(total_read));
    if (n <= 0) break;
    total_read += static_cast<std::size_t>(n);
  }
  ::close(fd);

  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset + kWalHeaderSize <= total_read) {
    const char* rec = data.data() + offset;
    uint32_t stored_crc = 0;
    uint32_t val_len = 0;
    uint64_t seq = 0;
    uint8_t raw_type = 0;

    std::memcpy(&stored_crc, rec, 4);
    std::memcpy(&val_len, rec + 4, 4);
    std::memcpy(&seq, rec + 8, 8);
    std::memcpy(&raw_type, rec + 16, 1);

    // Sanity check on length or torn record at tail
    if (val_len > 64ULL * 1024 * 1024 ||
        offset + kWalHeaderSize + val_len > total_read) {
      break;  // Stop at torn tail record
    }

    const std::size_t record_size = kWalHeaderSize + val_len;
    uint32_t expected_crc =
        crc32c::Mask(crc32c::Value(rec + 4, record_size - 4));
    if (stored_crc != expected_crc) {
      break;  // Stop at first corrupt record
    }

    ValueType type =
        (raw_type == static_cast<uint8_t>(ValueType::kDelete))
            ? ValueType::kDelete
            : ValueType::kPut;
    std::string_view key_view(rec + 17, kDefaultKeySize);
    std::string_view val_view(rec + kWalHeaderSize, val_len);

    memtable->Insert(seq, type, key_view, val_view);
    if (max_seq != nullptr && seq > *max_seq) {
      *max_seq = seq;
    }
    ++count;
    offset += record_size;
  }

  if (records_replayed != nullptr) {
    *records_replayed = count;
  }
  return Status::OK();
}

}  // namespace lsm
