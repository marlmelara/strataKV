#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace stratakv::testing {

// A unique temporary directory, removed when the object dies.
class TempDir {
 public:
  TempDir() {
    const std::string pattern =
        (std::filesystem::temp_directory_path() / "stratakv_test_XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) std::abort();
    path_ = created;
  }

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  std::string Child(std::string_view name) const { return path_ + "/" + std::string(name); }

 private:
  std::string path_;
};

inline std::span<const uint8_t> AsBytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// A key wide enough that 10,000 of them sort in the same order as their index,
// which keeps the recovery assertions readable.
inline std::string NumberedKey(int index) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "key-%08d", index);
  return buffer;
}

inline std::string NumberedValue(int index) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "value-%08d", index);
  return buffer;
}

}  // namespace stratakv::testing
