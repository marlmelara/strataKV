#include "wal_writer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "stratakv/status.h"

#include "crc32c.h"
#include "internal_key.h"

namespace stratakv {

WalWriter::WalWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}

// Appends one record. It is acknowledged only after Sync() returns, which is
// why the sync happens here rather than being left to the caller to remember.
Status WalWriter::AddRecord(std::span<const uint8_t> payload, bool sync) {
  if (payload.size() > kMaxWalRecordSize) {
    return Status::InvalidArgument("wal record", "payload too large");
  }

  std::array<uint8_t, kWalHeaderSize> header{};
  const uint8_t type = kWalFullRecord;

  // Checksum the type byte, then continue the same checksum over the payload,
  // so neither has to be copied into one buffer first.
  const uint32_t crc = crc32c::Extend(crc32c::Value(&type, 1), payload.data(), payload.size());
  EncodeFixed32(header.data(), crc);
  EncodeFixed32(header.data() + 4, static_cast<uint32_t>(payload.size()));
  header[8] = type;

  if (auto s = file_->Append({header.data(), header.size()}); !s.ok()) return s;
  if (auto s = file_->Append(payload); !s.ok()) return s;
  return sync ? file_->Sync() : Status::OK();
}

Status WalWriter::Sync() { return file_->Sync(); }

Status WalWriter::Close() { return file_->Close(); }

}  // namespace stratakv
