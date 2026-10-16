#include "wal_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "stratakv/status.h"

#include "crc32c.h"
#include "env.h"
#include "internal_key.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

// What reading one record at an offset produced.
enum class RecordState : uint8_t {
  kGood,        // Complete and the checksum matches.
  kIncomplete,  // The file ends inside this record: a torn write.
  kBadChecksum  // All the bytes are present but the checksum fails.
};

struct RecordRead {
  RecordState state = RecordState::kIncomplete;
  uint32_t length = 0;       // Payload length from the header.
  std::string_view payload;  // Valid only when state is kGood.
};

// Reads the record at `offset` into `buffer`.
//
// `buffer` holds the type byte followed by the payload, because that is exactly
// the range the writer checksummed.
Status ReadRecord(const RandomAccessFile& file, uint64_t offset, std::vector<uint8_t>* buffer,
                  RecordRead* out) {
  std::array<uint8_t, kWalHeaderSize> header{};
  size_t got = 0;
  if (auto s = file.ReadAt(offset, header.data(), header.size(), &got); !s.ok()) return s;
  if (got < kWalHeaderSize) {
    out->state = RecordState::kIncomplete;  // Torn header.
    return Status::OK();
  }

  const uint32_t crc = DecodeFixed32(header.data());
  const uint32_t length = DecodeFixed32(header.data() + 4);
  out->length = length;

  // A garbage length from a torn header could be billions; without this check
  // the next line would try to allocate that much.
  if (length > kMaxWalRecordSize) {
    out->state = RecordState::kIncomplete;
    return Status::OK();
  }

  buffer->assign(size_t{1} + length, 0);
  (*buffer)[0] = header[8];
  if (auto s = file.ReadAt(offset + kWalHeaderSize, buffer->data() + 1, length, &got); !s.ok()) {
    return s;
  }
  if (got < length) {
    out->state = RecordState::kIncomplete;  // Torn payload.
    return Status::OK();
  }

  if (crc32c::Value(buffer->data(), buffer->size()) != crc) {
    out->state = RecordState::kBadChecksum;
    return Status::OK();
  }

  out->state = RecordState::kGood;
  out->payload = std::string_view(reinterpret_cast<const char*>(buffer->data()) + 1, length);
  return Status::OK();
}

struct ScanResult {
  uint64_t valid_end = 0;    // End offset of the last good record.
  uint64_t truncate_at = 0;  // Where the usable prefix stops.
  bool corruption = false;   // A bad record had valid records after it.
};

// Walks the whole file to decide, before applying anything, how much of it is
// trustworthy. Applying as we went would mean replaying records that sit after
// mid-file damage, which must instead be refused outright.
Status Scan(const RandomAccessFile& file, ScanResult* result) {
  const uint64_t size = file.Size();
  std::vector<uint8_t> buffer;

  uint64_t offset = 0;
  bool bad_seen = false;
  uint64_t first_bad_offset = 0;

  while (offset < size) {
    RecordRead record;
    if (auto s = ReadRecord(file, offset, &buffer, &record); !s.ok()) return s;

    if (record.state == RecordState::kIncomplete) {
      // Bytes are missing, so this is the end of the file by definition.
      break;
    }

    const uint64_t next_offset = offset + kWalHeaderSize + record.length;

    if (record.state == RecordState::kBadChecksum) {
      // Every byte is present but damaged. Keep scanning: whether this is a
      // torn tail or mid-file corruption depends entirely on what follows.
      if (!bad_seen) {
        bad_seen = true;
        first_bad_offset = offset;
      }
      offset = next_offset;
      continue;
    }

    if (bad_seen) {
      // A good record after a bad one. The file was fully written and then
      // damaged, so this is corruption, not an interrupted write.
      result->corruption = true;
      return Status::OK();
    }

    result->valid_end = next_offset;
    offset = next_offset;
  }

  // Nothing valid followed the damage, so treat it as the tail.
  result->truncate_at = bad_seen ? first_bad_offset : result->valid_end;
  return Status::OK();
}

}  // namespace

namespace {

// Adapts the raw-payload interface to the batch interface: decodes the
// seq | count | operations header the WAL puts in every payload.
class BatchDecodingHandler final : public RecordPayloadHandler {
 public:
  BatchDecodingHandler(WalRecordHandler& handler, const std::string& path)
      : handler_(&handler), path_(&path) {}

  Status OnRecord(std::string_view payload) override {
    // payload = seq(8) | count(4) | operations
    if (payload.size() < 12) {
      return Status::Corruption(*path_, "record payload too short for a batch header");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(payload.data());
    const uint64_t sequence = DecodeFixed64(bytes);
    const uint32_t count = DecodeFixed32(bytes + 8);
    return handler_->OnBatch(sequence, count, payload.substr(12));
  }

 private:
  WalRecordHandler* handler_;
  const std::string* path_;
};

}  // namespace

Status WalReader::Replay(Env& env, const std::string& directory, const std::string& path,
                         WalRecordHandler& handler) {
  BatchDecodingHandler adapter(handler, path);
  return ReplayRecords(env, directory, path, adapter);
}

Status WalReader::ReplayRecords(Env& env, const std::string& directory, const std::string& path,
                                RecordPayloadHandler& handler) {
  if (!env.FileExists(path)) return Status::OK();

  std::unique_ptr<RandomAccessFile> file;
  if (auto s = env.NewRandomAccessFile(path, &file); !s.ok()) return s;

  ScanResult scan;
  if (auto s = Scan(*file, &scan); !s.ok()) return s;
  if (scan.corruption) {
    return Status::Corruption(path, "a valid record follows a failed checksum");
  }

  // Second pass: apply the prefix now known to be entirely good.
  std::vector<uint8_t> buffer;
  uint64_t offset = 0;
  while (offset < scan.valid_end) {
    RecordRead record;
    if (auto s = ReadRecord(*file, offset, &buffer, &record); !s.ok()) return s;
    if (record.state != RecordState::kGood) {
      return Status::Corruption(path, "record changed between scan and replay");
    }

    if (auto s = handler.OnRecord(record.payload); !s.ok()) return s;
    offset += kWalHeaderSize + record.length;
  }

  // Drop the torn tail. The truncation is synced, and so is the directory,
  // because the next append must start on a record boundary even if the
  // process dies again immediately.
  const uint64_t size = file->Size();
  if (scan.truncate_at < size) {
    file.reset();  // Close before truncating.
    if (auto s = env.TruncateFile(path, scan.truncate_at); !s.ok()) return s;
    if (auto s = env.SyncFile(path); !s.ok()) return s;
    if (auto s = env.SyncDir(directory); !s.ok()) return s;
  }
  return Status::OK();
}

}  // namespace stratakv
