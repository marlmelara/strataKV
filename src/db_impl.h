#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "env.h"
#include "memtable.h"
#include "wal_writer.h"

namespace stratakv {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string path);
  ~DBImpl() override;

  // Creates or opens the directory, takes the LOCK file, replays the WAL, and
  // reopens it for appending.
  Status Recover();

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, WriteBatch* batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  Status Close() override;

 private:
  const Options options_;
  const std::string path_;
  Env& env_;

  std::unique_ptr<FileLock> lock_;

  // Guards every field below. Held only briefly on the read path: long enough
  // to copy a pointer and a sequence number, never across disk I/O.
  mutable std::mutex mutex_;

  // A shared_ptr even though P1 never replaces it, because from P2 a reader
  // keeps the old memtable alive while a flush installs a new one.
  std::shared_ptr<MemTable> mem_;
  std::unique_ptr<WalWriter> wal_;
  uint64_t last_sequence_ = 0;
  bool closed_ = false;
};

}  // namespace stratakv
