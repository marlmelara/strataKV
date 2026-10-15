#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "stratakv/status.h"

#include "env.h"

namespace stratakv {

// Receives each batch found in the WAL, in order.
class WalRecordHandler {
 public:
  WalRecordHandler() = default;
  virtual ~WalRecordHandler() = default;

  WalRecordHandler(const WalRecordHandler&) = delete;
  WalRecordHandler& operator=(const WalRecordHandler&) = delete;

  // `ops` is the encoded operation list; `sequence` is the number assigned to
  // its first operation.
  virtual Status OnBatch(uint64_t sequence, uint32_t count, std::string_view ops) = 0;
};

// Replays a write-ahead log, then cuts off anything that was not complete.
//
// The central rule, and the reason recovery is correct: a record that was not
// fully written was never acknowledged, so no caller was ever promised it.
// Truncating it away is not data loss. The checksum is what tells
// "half-written" apart from "complete".
//
// A bad record at the very end is therefore normal after a crash and is NOT an
// error. A bad record with valid records after it is a different thing
// entirely: that is damage in the middle of a file that was fully written, and
// it is reported as Corruption rather than silently truncated.
class WalReader {
 public:
  // Replays `path`, handing each complete batch to `handler`. Truncates a torn
  // tail in place and syncs the file and its directory so the truncation is
  // itself durable.
  //
  // Returns OK if the file is missing: an empty database has no log yet.
  static Status Replay(Env& env, const std::string& directory, const std::string& path,
                       WalRecordHandler& handler);
};

}  // namespace stratakv
