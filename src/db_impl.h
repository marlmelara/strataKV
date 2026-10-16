#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "compaction.h"
#include "env.h"
#include "memtable.h"
#include "version.h"
#include "wal_writer.h"

namespace stratakv {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string path);
  ~DBImpl() override;

  // Takes the LOCK file, recovers the version set from CURRENT and the
  // MANIFEST, replays any log newer than the last one the MANIFEST accounts
  // for, and starts the background thread.
  Status Recover();

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, WriteBatch* batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  Status Close() override;

  // Test-only observation points. Flushes and compactions are asynchronous, so
  // a test that wants to see their effects has to be able to wait.
  Status TEST_WaitForBackgroundWork();
  Status TEST_CompactNow();
  size_t TEST_FileCount(int level) const;
  uint64_t TEST_TotalFileBytes() const;
  CompactionStats TEST_LastCompactionStats() const;

 private:
  friend class ReadSnapshotGuard;

  // One waiting writer. It lives on its own thread's stack; other threads reach
  // it through the pointer in writers_.
  struct Writer {
    WriteBatch* batch = nullptr;
    bool sync = false;
    bool done = false;  // Set by a leader that wrote this batch on our behalf.
    Status status;
    std::condition_variable cv;
  };

  // Merges the batches queued behind the leader into one record, and returns
  // the last writer included. Called with the lock held.
  Writer* BuildBatchGroup(std::string* operations, uint32_t* count, bool* sync);

  void BackgroundLoop();

  // Writes `table` to a new L0 SSTable and records it in the MANIFEST. Runs
  // without the lock held.
  Status FlushImmutableMemTable();

  // Merges L0 into L1 and records the swap. Runs without the lock held.
  Status RunCompaction();

  // Rotates the memtable if it is full, waiting if a previous flush is still in
  // flight. Called with the lock held; may release it while waiting.
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock);

  // Records the current sequence number in a freshly created log as a
  // zero-operation record, so sequence numbering survives every log being
  // flushed away. Kept from P2 even though the MANIFEST now also records the
  // last sequence, because a log may hold writes the MANIFEST has not seen.
  Status WriteSequenceWatermark(WalWriter& wal);

  // Deletes files no live version references and logs no longer needed. Called
  // with the lock held; does its I/O with the lock released.
  void RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock);

  // The oldest sequence number a read in flight might still need. Compaction
  // must not drop any version above it.
  uint64_t SmallestActiveSnapshot() const;
  void UnregisterReadSnapshot(uint64_t sequence);

  const Options options_;
  const std::string path_;
  Env& env_;

  std::unique_ptr<FileLock> lock_;

  mutable std::mutex mutex_;
  std::condition_variable background_work_;  // Signals the background thread.
  std::condition_variable background_done_;  // Signals waiting writers.

  std::unique_ptr<VersionSet> versions_;

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // Full, awaiting flush. Null when idle.
  std::unique_ptr<WalWriter> wal_;

  // The logs whose records make up each memtable. A memtable's logs may only be
  // deleted once its table is recorded in the MANIFEST, and recovery may have
  // replayed several logs into one memtable.
  std::vector<uint64_t> mem_wal_numbers_;
  std::vector<uint64_t> imm_wal_numbers_;

  // Writers in arrival order. The one at the front is the leader: it does the
  // log append and the fsync for everyone it merges, and nobody else writes
  // while it holds that position.
  std::deque<Writer*> writers_;

  // Snapshots of reads currently in flight. A multiset because several reads
  // may share a sequence number, and each must be released independently.
  std::multiset<uint64_t> active_read_snapshots_;

  std::thread background_thread_;
  bool shutting_down_ = false;
  bool background_busy_ = false;
  bool compaction_requested_ = false;
  Status background_error_;
  CompactionStats last_compaction_stats_;
  bool closed_ = false;
};

}  // namespace stratakv
