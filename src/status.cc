#include "stratakv/status.h"

#include <string>
#include <string_view>

namespace stratakv {
namespace {

std::string_view CodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotFound:
      return "NotFound";
    case StatusCode::kCorruption:
      return "Corruption";
    case StatusCode::kIOError:
      return "IOError";
    case StatusCode::kInvalidArgument:
      return "InvalidArgument";
  }
  return "Unknown";
}

}  // namespace

Status::Status(StatusCode code, std::string_view context, std::string_view detail)
    : code_(code), message_(context) {
  if (!detail.empty()) {
    if (!message_.empty()) message_ += ": ";
    message_ += detail;
  }
}

std::string Status::ToString() const {
  std::string out(CodeName(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace stratakv
