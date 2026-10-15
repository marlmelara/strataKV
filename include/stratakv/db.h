#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

namespace stratakv {

// An embedded key-value database.
//
// The guarantee: if Put returns OK, that write survives a crash. Keys and
// values are arbitrary bytes, including zeros.
class DB {
 public:
  // Opens or creates the database at `path`, taking an exclusive lock on it so
  // two processes cannot open the same directory. Replays the write-ahead log,
  // discarding any record that was not completely written.
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);

  DB() = default;
  virtual ~DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions& options, std::string_view key, std::string_view value) = 0;

  // Writes a tombstone. The key's old value is not erased yet; it is shadowed
  // by this newer entry and removed later by compaction.
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;

  // Applies every operation in `batch` atomically: one WAL record, one
  // checksum, one fsync.
  virtual Status Write(const WriteOptions& options, WriteBatch* batch) = 0;

  // Returns NotFound if the key is absent or deleted.
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

  // Flushes and releases the lock. Also run by the destructor, but calling it
  // explicitly is the only way to see an error from the final sync.
  virtual Status Close() = 0;
};

}  // namespace stratakv
