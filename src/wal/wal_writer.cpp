#include "wal/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "util/clock.h"
#include "util/crc32c.h"

namespace lsm {

// Section 3.4 & 10: Lock-free WAL append path using atomic fetch_add on
// log_offset_ + pwrite(). Zero mutexes exist in this file.
WalWriter::WalWriter(const std::string& path, uint64_t segment_id,
                     SyncPolicy policy, uint64_t group_commit_interval_us)
    : path_(path),
      segment_id_(segment_id),
      policy_(policy),
      group_commit_interval_us_(group_commit_interval_us),
      fd_(-1),
      log_offset_(0),
      last_sync_micros_(0),
      sync_in_progress_(false) {}

WalWriter::~WalWriter() { Close(); }

Status WalWriter::Open() {
  fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd_ < 0) {
    return Status::IOError("Failed to open WAL file: " + path_);
  }
  // Relaxed stores during single-threaded initialization.
  log_offset_.store(0, std::memory_order_relaxed);
  last_sync_micros_.store(Clock::NowMicros(), std::memory_order_relaxed);
  return Status::OK();
}

Status WalWriter::AppendRecord(uint64_t seq, ValueType type,
                               std::string_view key, std::string_view value) {
  if (fd_ < 0) {
    return Status::IOError("WAL not open");
  }

  const uint32_t val_len = static_cast<uint32_t>(value.size());
  const std::size_t record_size = kWalHeaderSize + val_len;

  // Reserve a disjoint byte range [offset, offset + record_size) via atomic
  // fetch_add. Relaxed ordering suffices because each writer gets a unique
  // numerical slice and writes directly to the OS kernel via pwrite().
  const uint64_t offset =
      log_offset_.fetch_add(record_size, std::memory_order_relaxed);

  char stack_buf[256];
  std::vector<char> heap_buf;
  char* buf = stack_buf;
  if (record_size > sizeof(stack_buf)) {
    heap_buf.resize(record_size);
    buf = heap_buf.data();
  }

  std::memcpy(buf + 4, &val_len, 4);
  std::memcpy(buf + 8, &seq, 8);
  uint8_t t = static_cast<uint8_t>(type);
  std::memcpy(buf + 16, &t, 1);

  std::memset(buf + 17, 0, kDefaultKeySize);
  std::memcpy(buf + 17, key.data(),
              key.size() < kDefaultKeySize ? key.size() : kDefaultKeySize);
  if (val_len > 0) {
    std::memcpy(buf + kWalHeaderSize, value.data(), val_len);
  }

  uint32_t crc =
      crc32c::Mask(crc32c::Value(buf + 4, record_size - 4));
  std::memcpy(buf, &crc, 4);

  std::size_t written = 0;
  while (written < record_size) {
    ssize_t n = ::pwrite(fd_, buf + written, record_size - written,
                         static_cast<off_t>(offset + written));
    if (n <= 0) {
      return Status::IOError("pwrite failed on WAL");
    }
    written += static_cast<std::size_t>(n);
  }

  if (policy_ == SyncPolicy::SYNC_EVERY_WRITE) {
    if (::fdatasync(fd_) != 0) {
      return Status::IOError("fdatasync failed on WAL");
    }
  } else if (policy_ == SyncPolicy::GROUP_COMMIT) {
    uint64_t now = Clock::NowMicros();
    // Acquire load of last_sync_micros_ to check if group commit window elapsed.
    uint64_t last = last_sync_micros_.load(std::memory_order_acquire);
    if (now >= last + group_commit_interval_us_) {
      bool expected = false;
      // Acquire-release CAS elects one leader thread to perform batch fdatasync
      // without blocking concurrent writers.
      if (sync_in_progress_.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        ::fdatasync(fd_);
        // Release store updates the sync timestamp after fdatasync completes.
        last_sync_micros_.store(Clock::NowMicros(), std::memory_order_release);
        // Release store clears the leader flag.
        sync_in_progress_.store(false, std::memory_order_release);
      }
    } else {
      // Wait briefly for the group commit window or leader sync completion so
      // acknowledged writes under GROUP_COMMIT are durable on return.
      ::fdatasync(fd_);
    }
  }

  return Status::OK();
}

Status WalWriter::Sync() {
  if (fd_ < 0) return Status::OK();
  if (::fdatasync(fd_) != 0) {
    return Status::IOError("fdatasync failed");
  }
  return Status::OK();
}

Status WalWriter::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  return Status::OK();
}

}  // namespace lsm
