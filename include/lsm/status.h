#ifndef LSM_INCLUDE_LSM_STATUS_H_
#define LSM_INCLUDE_LSM_STATUS_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

class Status {
 public:
  enum Code : uint8_t {
    kOk = 0,
    kNotFound = 1,
    kIOError = 2,
    kCorruption = 3,
    kInvalidArgument = 4,
  };

  Status() noexcept : code_(kOk), msg_() {}
  Status(Code code, std::string msg = std::string()) noexcept
      : code_(code), msg_(std::move(msg)) {}

  static Status OK() noexcept { return Status(); }
  static Status NotFound(std::string msg = "NotFound") noexcept {
    return Status(kNotFound, std::move(msg));
  }
  static Status IOError(std::string msg = "IOError") noexcept {
    return Status(kIOError, std::move(msg));
  }
  static Status Corruption(std::string msg = "Corruption") noexcept {
    return Status(kCorruption, std::move(msg));
  }
  static Status InvalidArgument(std::string msg = "InvalidArgument") noexcept {
    return Status(kInvalidArgument, std::move(msg));
  }

  bool ok() const noexcept { return code_ == kOk; }
  bool IsNotFound() const noexcept { return code_ == kNotFound; }

  std::string ToString() const {
    switch (code_) {
      case kOk: return "OK";
      case kNotFound: return msg_.empty() ? "NotFound" : "NotFound: " + msg_;
      case kIOError: return msg_.empty() ? "IOError" : "IOError: " + msg_;
      case kCorruption: return msg_.empty() ? "Corruption" : "Corruption: " + msg_;
      case kInvalidArgument:
        return msg_.empty() ? "InvalidArgument" : "InvalidArgument: " + msg_;
      default: return "UnknownStatus";
    }
  }

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.code_ == rhs.code_ && lhs.msg_ == rhs.msg_;
  }

  friend bool operator!=(const Status& lhs, const Status& rhs) noexcept {
    return !(lhs == rhs);
  }

 private:
  Code code_;
  std::string msg_;
};

}  // namespace lsm

#endif  // LSM_INCLUDE_LSM_STATUS_H_
