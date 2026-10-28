#include "version/version.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

#include "util/crc32c.h"

namespace lsm {

namespace {

void PutU32(std::string* dst, uint32_t v) {
  dst->append(reinterpret_cast<const char*>(&v), 4);
}

void PutU64(std::string* dst, uint64_t v) {
  dst->append(reinterpret_cast<const char*>(&v), 8);
}

void PutInternalKey(std::string* dst, const InternalKey& k) {
  dst->append(k.user_key, kDefaultKeySize);
  PutU64(dst, k.seq);
  uint8_t t = static_cast<uint8_t>(k.type);
  dst->append(reinterpret_cast<const char*>(&t), 1);
}

bool GetU32(std::string_view* src, uint32_t* v) {
  if (src->size() < 4) return false;
  std::memcpy(v, src->data(), 4);
  src->remove_prefix(4);
  return true;
}

bool GetU64(std::string_view* src, uint64_t* v) {
  if (src->size() < 8) return false;
  std::memcpy(v, src->data(), 8);
  src->remove_prefix(8);
  return true;
}

bool GetInternalKey(std::string_view* src, InternalKey* k) {
  if (src->size() < kDefaultKeySize + 9) return false;
  std::memcpy(k->user_key, src->data(), kDefaultKeySize);
  src->remove_prefix(kDefaultKeySize);
  if (!GetU64(src, &k->seq)) return false;
  uint8_t t = static_cast<uint8_t>((*src)[0]);
  src->remove_prefix(1);
  k->type = (t == static_cast<uint8_t>(ValueType::kDelete)) ? ValueType::kDelete
                                                            : ValueType::kPut;
  return true;
}

std::string EncodeEdit(const VersionEdit& edit) {
  std::string payload;
  uint8_t flags = (edit.has_next_file_number ? 1 : 0) |
                  (edit.has_last_sequence ? 2 : 0) |
                  (edit.has_min_log_number ? 4 : 0);
  payload.push_back(static_cast<char>(flags));
  PutU64(&payload, edit.next_file_number);
  PutU64(&payload, edit.last_sequence);
  PutU64(&payload, edit.min_log_number);

  PutU32(&payload, static_cast<uint32_t>(edit.deleted_files.size()));
  for (const auto& del : edit.deleted_files) {
    PutU32(&payload, static_cast<uint32_t>(del.first));
    PutU64(&payload, del.second);
  }

  PutU32(&payload, static_cast<uint32_t>(edit.added_files.size()));
  for (const auto& add : edit.added_files) {
    PutU32(&payload, static_cast<uint32_t>(add.first));
    PutU64(&payload, add.second.file_number);
    PutU64(&payload, add.second.file_size);
    PutU64(&payload, add.second.num_entries);
    PutInternalKey(&payload, add.second.smallest);
    PutInternalKey(&payload, add.second.largest);
    PutU64(&payload, add.second.max_seq);
  }

  std::string rec;
  uint32_t len = static_cast<uint32_t>(payload.size());
  uint32_t crc = crc32c::Mask(crc32c::Value(payload.data(), payload.size()));
  PutU32(&rec, crc);
  PutU32(&rec, len);
  rec.append(payload);
  return rec;
}

bool DecodeEdit(std::string_view payload, VersionEdit* edit) {
  if (payload.empty()) return false;
  uint8_t flags = static_cast<uint8_t>(payload[0]);
  payload.remove_prefix(1);
  edit->has_next_file_number = (flags & 1) != 0;
  edit->has_last_sequence = (flags & 2) != 0;
  edit->has_min_log_number = (flags & 4) != 0;

  if (!GetU64(&payload, &edit->next_file_number)) return false;
  if (!GetU64(&payload, &edit->last_sequence)) return false;
  if (!GetU64(&payload, &edit->min_log_number)) return false;

  uint32_t num_del = 0;
  if (!GetU32(&payload, &num_del)) return false;
  edit->deleted_files.clear();
  for (uint32_t i = 0; i < num_del; ++i) {
    uint32_t lvl = 0;
    uint64_t fnum = 0;
    if (!GetU32(&payload, &lvl) || !GetU64(&payload, &fnum)) return false;
    edit->deleted_files.emplace_back(static_cast<int>(lvl), fnum);
  }

  uint32_t num_add = 0;
  if (!GetU32(&payload, &num_add)) return false;
  edit->added_files.clear();
  for (uint32_t i = 0; i < num_add; ++i) {
    uint32_t lvl = 0;
    FileMetaData meta;
    if (!GetU32(&payload, &lvl) || !GetU64(&payload, &meta.file_number) ||
        !GetU64(&payload, &meta.file_size) ||
        !GetU64(&payload, &meta.num_entries) ||
        !GetInternalKey(&payload, &meta.smallest) ||
        !GetInternalKey(&payload, &meta.largest) ||
        !GetU64(&payload, &meta.max_seq)) {
      return false;
    }
    edit->added_files.emplace_back(static_cast<int>(lvl), std::move(meta));
  }
  return true;
}

}  // namespace

ManifestLog::ManifestLog(const std::string& db_path)
    : db_path_(db_path),
      manifest_filename_("MANIFEST-000001"),
      fd_(-1) {}

ManifestLog::~ManifestLog() {
  if (fd_ >= 0) ::close(fd_);
}

Status ManifestLog::OpenOrRecover(std::vector<VersionEdit>* edits_out) {
  std::string current_path = db_path_ + "/CURRENT";
  std::ifstream cur_in(current_path);
  if (cur_in.is_open()) {
    std::getline(cur_in, manifest_filename_);
    cur_in.close();
  } else {
    manifest_filename_ = "MANIFEST-000001";
    std::ofstream cur_out(current_path, std::ios::trunc);
    cur_out << manifest_filename_ << "\n";
    cur_out.close();
  }

  std::string full_path = db_path_ + "/" + manifest_filename_;
  fd_ = ::open(full_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd_ < 0) {
    return Status::IOError("Failed to open MANIFEST: " + full_path);
  }

  struct stat st {};
  if (::fstat(fd_, &st) == 0 && st.st_size > 0) {
    std::size_t sz = static_cast<std::size_t>(st.st_size);
    std::string data(sz, '\0');
    if (::pread(fd_, data.data(), sz, 0) == static_cast<ssize_t>(sz)) {
      std::size_t off = 0;
      while (off + 8 <= sz) {
        uint32_t crc = 0, len = 0;
        std::memcpy(&crc, data.data() + off, 4);
        std::memcpy(&len, data.data() + off + 4, 4);
        if (off + 8 + len > sz) break;
        uint32_t actual =
            crc32c::Mask(crc32c::Value(data.data() + off + 8, len));
        if (crc != actual) break;
        VersionEdit edit;
        if (DecodeEdit(std::string_view(data.data() + off + 8, len), &edit)) {
          edits_out->push_back(std::move(edit));
        }
        off += 8 + len;
      }
    }
  }
  ::lseek(fd_, 0, SEEK_END);
  return Status::OK();
}

Status ManifestLog::AppendEdit(const VersionEdit& edit) {
  if (fd_ < 0) return Status::IOError("MANIFEST not open");
  std::string rec = EncodeEdit(edit);
  ssize_t n = ::write(fd_, rec.data(), rec.size());
  if (n != static_cast<ssize_t>(rec.size())) {
    return Status::IOError("Failed to write MANIFEST edit");
  }
  ::fdatasync(fd_);
  return Status::OK();
}

}  // namespace lsm
