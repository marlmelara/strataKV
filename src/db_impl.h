#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "env.h"
#include "memtable.h"
#include "sstable_reader.h"
#include "wal_writer.h"

namespace stratakv {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string path);
  ~DBImpl() override;

  // Opens the directory, takes the LOCK file, discovers existing SSTables and
  // logs, replays the logs, and starts the background flush thread.
  Status Recover();

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, WriteBatch* batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  Status Close() override;

  // Test-only observation points. The flush is asynchronous, so a test that
  // wants to see its effects has to be able to wait for it.
  Status TEST_WaitForFlush();
  size_t TEST_Level0FileCount() const;

 private:
  // A snapshot of the L0 files, newest first. Copied under the lock by a reader
  // and then used without it, so the files it needs stay open even if the
  // background thread installs a new set meanwhile. P3 generalizes this into a
  // proper Version object.
  using TableSet = std::vector<std::shared_ptr<SSTableReader>>;

  void BackgroundLoop();

  // Writes `table` to a new SSTable, syncs it and the directory, and opens a
  // reader for it. Runs without the lock held.
  Status WriteLevel0Table(const MemTable& table, uint64_t table_number,
                          std::shared_ptr<SSTableReader>* reader);

  // Rotates the memtable if it is full, waiting if a previous flush is still
  // in flight. Called with the lock held; may release it while waiting.
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);

  // Records the current sequence number in a freshly created log as a
  // zero-operation record.
  //
  // Without this, flushing every memtable and deleting its log would leave a
  // new empty log with nothing to say how far sequence numbers had got, and
  // recovery would restart numbering from zero. The alternative is reading
  // every SSTable at open just to find the highest sequence.
  Status WriteSequenceWatermark(WalWriter& wal);

  const Options options_;
  const std::string path_;
  Env& env_;

  std::unique_ptr<FileLock> lock_;

  mutable std::mutex mutex_;
  std::condition_variable background_work_;  // Signals the flush thread.
  std::condition_variable background_done_;  // Signals waiting writers.

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // Full, awaiting flush. Null when idle.
  std::unique_ptr<WalWriter> wal_;

  // The logs whose records make up each memtable. A memtable's logs may only be
  // deleted once its SSTable is durable, and recovery may have replayed several
  // logs into one memtable, so this is a list rather than a single number.
  std::vector<uint64_t> mem_wal_numbers_;
  std::vector<uint64_t> imm_wal_numbers_;

  uint64_t next_file_number_ = 1;
  uint64_t last_sequence_ = 0;

  std::shared_ptr<const TableSet> l0_;

  std::thread background_thread_;
  bool shutting_down_ = false;
  bool flush_in_progress_ = false;
  Status background_error_;
  bool closed_ = false;
};

}  // namespace stratakv
