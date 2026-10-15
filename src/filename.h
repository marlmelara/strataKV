#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The names of a database's files, and how to read them back.
//
// P2 has no MANIFEST, so recovery discovers what exists by listing the
// directory and parsing these names. That is also where the next file number
// comes from: one past the highest number seen. P3 replaces this with the
// MANIFEST, which records the file set explicitly.
namespace stratakv {

enum class FileType : uint8_t {
  kWal,    // 000007.log -- a write-ahead log
  kTable,  // 000004.sst -- an SSTable
};

inline std::string LockFileName(const std::string& directory) { return directory + "/LOCK"; }

namespace internal {

// Zero-padded to six digits so the files sort in the order they were created.
inline std::string NumberedName(const std::string& directory, uint64_t number,
                                std::string_view suffix) {
  std::string digits = std::to_string(number);
  if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
  return directory + "/" + digits + std::string(suffix);
}

}  // namespace internal

inline std::string WalFileName(const std::string& directory, uint64_t number) {
  return internal::NumberedName(directory, number, ".log");
}

inline std::string TableFileName(const std::string& directory, uint64_t number) {
  return internal::NumberedName(directory, number, ".sst");
}

// Parses a bare file name such as "000007.log". Returns false for anything that
// is not one of ours -- LOCK, a stray file, or a number that does not fit --
// so an unexpected file in the directory is ignored rather than misread.
inline bool ParseFileName(std::string_view name, uint64_t* number, FileType* type) {
  if (name.size() > 4 && name.substr(name.size() - 4) == ".log") {
    *type = FileType::kWal;
  } else if (name.size() > 4 && name.substr(name.size() - 4) == ".sst") {
    *type = FileType::kTable;
  } else {
    return false;
  }

  const std::string_view digits = name.substr(0, name.size() - 4);
  if (digits.empty() || digits.size() > 20) return false;

  uint64_t parsed = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
    // Guard against a name whose digits would overflow a 64-bit number.
    if (parsed > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) return false;
    parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = parsed;
  return true;
}

}  // namespace stratakv
