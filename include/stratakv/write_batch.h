#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "stratakv/status.h"

namespace stratakv {

// A group of writes applied atomically.
//
// The whole batch becomes one WAL record with one checksum, so after a crash it
// replays entirely or not at all. That is how StrataKV makes multi-key writes
// atomic without a transaction manager.
class WriteBatch {
 public:
  // Receives the operations in a batch, in order, during replay or when a batch
  // is inserted into a memtable.
  class Handler {
   public:
    Handler() = default;
    virtual ~Handler() = default;

    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;

    virtual Status Put(std::string_view key, std::string_view value) = 0;
    virtual Status Delete(std::string_view key) = 0;
  };

  WriteBatch() = default;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  // Number of operations, written into the WAL record so replay knows how many
  // to decode.
  uint32_t Count() const { return count_; }

  bool empty() const { return count_ == 0; }

  // The encoded operations, without any sequence number or count: those are
  // added by the WAL record that carries the batch.
  std::string_view Contents() const { return rep_; }

  // Decodes `rep_` and hands each operation to `handler`. Returns Corruption if
  // the bytes do not decode cleanly, which matters because the same routine
  // replays records read back from disk.
  Status Iterate(Handler& handler) const;

  // Decodes an arbitrary encoded operation list, used by WAL replay where the
  // bytes came from a file rather than from a live WriteBatch.
  static Status IterateContents(std::string_view contents, uint32_t count, Handler& handler);

 private:
  std::string rep_;  // A byte buffer, not text: it may contain zero bytes.
  uint32_t count_ = 0;
};

}  // namespace stratakv
