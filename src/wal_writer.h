#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "stratakv/status.h"

#include "env.h"

namespace stratakv {

// WAL record framing (little-endian):
//
//   +-----------+-----------+-----------+------------------------------+
//   | crc32c(4) | length(4) | type(1)   | payload (length bytes)       |
//   +-----------+-----------+-----------+------------------------------+
//
// The checksum covers the type byte plus the payload. One record is one
// WriteBatch, which is what makes a multi-key write atomic: the batch replays
// entirely or not at all.
inline constexpr size_t kWalHeaderSize = 9;
inline constexpr uint8_t kWalFullRecord = 1;

// A length larger than this means a torn or garbage header. Without the check,
// a corrupt length could ask the reader to allocate gigabytes.
inline constexpr uint32_t kMaxWalRecordSize = 64U << 20;

// Appends records to the write-ahead log.
class WalWriter {
 public:
  explicit WalWriter(std::unique_ptr<WritableFile> file);

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Appends one record. When `sync` is true the data is forced to durable
  // storage before returning, and only then may the write be acknowledged to
  // the caller.
  Status AddRecord(std::span<const uint8_t> payload, bool sync);

  Status Sync();
  Status Close();

  uint64_t Size() const { return file_->Size(); }

 private:
  std::unique_ptr<WritableFile> file_;
};

}  // namespace stratakv
