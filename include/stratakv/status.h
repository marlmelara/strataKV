#pragma once

#include <string>
#include <string_view>

namespace stratakv {

// Why Status instead of exceptions: every engine call that can fail returns a
// Status, so failure is visible at the call site and there is no hidden control
// flow on the write path. LevelDB and RocksDB make the same choice. The binding
// layer turns a bad Status into a Python exception, which is where exceptions
// belong.
enum class StatusCode {
  kOk = 0,
  kNotFound,
  kCorruption,
  kIOError,
  kInvalidArgument,
};

// Marked [[nodiscard]] on purpose: the cost of returning errors instead of
// throwing them is that every result must be checked, so the compiler enforces
// it. To ignore one deliberately, cast it to void.
class [[nodiscard]] Status {
 public:
  // A default-constructed Status is success, so `return {};` means OK.
  Status() = default;

  static Status OK() { return Status(); }

  // `context` names what failed (usually a path or key); `detail` carries the
  // reason (usually strerror). Both are copied into the message eagerly, which
  // keeps Status self-contained and safe to return up the stack.
  static Status NotFound(std::string_view context, std::string_view detail = {}) {
    return Status(StatusCode::kNotFound, context, detail);
  }
  static Status Corruption(std::string_view context, std::string_view detail = {}) {
    return Status(StatusCode::kCorruption, context, detail);
  }
  static Status IOError(std::string_view context, std::string_view detail = {}) {
    return Status(StatusCode::kIOError, context, detail);
  }
  static Status InvalidArgument(std::string_view context, std::string_view detail = {}) {
    return Status(StatusCode::kInvalidArgument, context, detail);
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
  bool IsCorruption() const { return code_ == StatusCode::kCorruption; }
  bool IsIOError() const { return code_ == StatusCode::kIOError; }
  bool IsInvalidArgument() const { return code_ == StatusCode::kInvalidArgument; }

  StatusCode code() const { return code_; }

  // The human-readable part, without the code name. Empty for OK.
  std::string_view message() const { return message_; }

  // "OK", or "IOError: /tmp/mydb/CURRENT: No such file or directory".
  std::string ToString() const;

 private:
  Status(StatusCode code, std::string_view context, std::string_view detail);

  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace stratakv
