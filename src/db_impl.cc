#include "db_impl.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "compaction.h"
#include "crash_point.h"
#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "memtable.h"
#include "sstable_builder.h"
#include "version.h"
#include "wal_reader.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

// An upper bound on a merged group, so a very large batch at the front cannot
// make every writer behind it wait for megabytes of I/O.
constexpr size_t kGroupCommitMaxBytes = 1 << 20;

// When the leader's own batch is small, keep the group small too: a short write
// should not be delayed by a group that grew to the full limit behind it.
constexpr size_t kGroupCommitSmallBatchBytes = 128 << 10;

std::span<const uint8_t> AsBytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// Builds the payload the WAL carries: seq(8) | count(4) | operations.
std::string EncodeRecordPayload(uint64_t sequence, uint32_t count, std::string_view ops) {
  std::string payload;
  payload.reserve(12 + ops.size());
  PutFixed64(&payload, sequence);
  PutFixed32(&payload, count);
  payload.append(ops);
  return payload;
}

// Inserts a batch's operations into a memtable, numbering them consecutively
// from the batch's base sequence.
class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(uint64_t sequence, MemTable* memtable)
      : sequence_(sequence), memtable_(memtable) {}

  Status Put(std::string_view key, std::string_view value) override {
    memtable_->Add(sequence_++, ValueType::kTypeValue, key, value);
    return Status::OK();
  }

  Status Delete(std::string_view key) override {
    memtable_->Add(sequence_++, ValueType::kTypeDeletion, key, {});
    return Status::OK();
  }

  uint64_t next_sequence() const { return sequence_; }

 private:
  uint64_t sequence_;
  MemTable* memtable_;
};

// Rebuilds a memtable from WAL records during recovery.
class RecoveryHandler final : public WalRecordHandler {
 public:
  RecoveryHandler(MemTable* memtable, uint64_t* last_sequence)
      : memtable_(memtable), last_sequence_(last_sequence) {}

  Status OnBatch(uint64_t sequence, uint32_t count, std::string_view ops) override {
    MemTableInserter inserter(sequence, memtable_);
    if (auto s = WriteBatch::IterateContents(ops, count, inserter); !s.ok()) return s;
    // A zero-operation record is a sequence watermark: no data, but its
    // sequence still says how far numbering had reached.
    const uint64_t highest = inserter.next_sequence() - 1;
    if (highest > *last_sequence_) *last_sequence_ = highest;
    return Status::OK();
  }

 private:
  MemTable* memtable_;
  uint64_t* last_sequence_;
};

}  // namespace

// Releases a read's registered snapshot however the read exits, including the
// several early returns in Get.
class ReadSnapshotGuard {
 public:
  ReadSnapshotGuard(DBImpl* db, uint64_t sequence) : db_(db), sequence_(sequence) {}
  ~ReadSnapshotGuard() { db_->UnregisterReadSnapshot(sequence_); }

  ReadSnapshotGuard(const ReadSnapshotGuard&) = delete;
  ReadSnapshotGuard& operator=(const ReadSnapshotGuard&) = delete;

 private:
  DBImpl* db_;
  uint64_t sequence_;
};

DBImpl::DBImpl(const Options& options, std::string path)
    : options_(options), path_(std::move(path)), env_(Env::Default()) {}

DBImpl::~DBImpl() {
  // A destructor must not let anything escape, and there is nowhere to report a
  // failure anyway -- which is exactly why Close() exists as an explicit call
  // for anyone who needs to see the error from the final sync.
  try {
    (void)Close();
  } catch (...) {
  }
}

Status DBImpl::WriteSequenceWatermark(WalWriter& wal) {
  const std::string payload = EncodeRecordPayload(versions_->last_sequence() + 1, 0, {});
  return wal.AddRecord(AsBytes(payload), /*sync=*/true);
}

uint64_t DBImpl::SmallestActiveSnapshot() const {
  // With no read in flight, everything up to the newest write may be collapsed.
  if (active_read_snapshots_.empty()) return versions_->last_sequence();
  return *active_read_snapshots_.begin();
}

void DBImpl::UnregisterReadSnapshot(uint64_t sequence) {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto it = active_read_snapshots_.find(sequence);
  if (it != active_read_snapshots_.end()) active_read_snapshots_.erase(it);
}

Status DBImpl::Recover() {
  const bool existed = env_.FileExists(path_);
  if (!existed && !options_.create_if_missing) {
    return Status::InvalidArgument(path_, "does not exist and create_if_missing is false");
  }
  if (existed && options_.error_if_exists) {
    return Status::InvalidArgument(path_, "exists and error_if_exists is true");
  }
  if (auto s = env_.CreateDirIfMissing(path_); !s.ok()) return s;

  // Taken before touching any data file, so a second process fails fast
  // instead of corrupting the first one's logs.
  if (auto s = env_.LockFile(LockFileName(path_), &lock_); !s.ok()) return s;

  versions_ = std::make_unique<VersionSet>(env_, path_, options_);
  bool created = false;
  if (auto s = versions_->Recover(&created); !s.ok()) return s;

  // Logs at or above the recorded log number hold writes the MANIFEST does not
  // account for; anything older was already folded into a table.
  std::vector<std::string> children;
  if (auto s = env_.GetChildren(path_, &children); !s.ok()) return s;

  std::vector<uint64_t> wal_numbers;
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (!ParseFileName(name, &number, &type)) continue;
    if (type == FileType::kWal && number >= versions_->log_number()) {
      wal_numbers.push_back(number);
    }
    versions_->MarkFileNumberUsed(number);
  }
  std::sort(wal_numbers.begin(), wal_numbers.end());

  mem_ = std::make_shared<MemTable>();

  // Replay oldest first so later versions of a key land on top of earlier ones.
  uint64_t last_sequence = versions_->last_sequence();
  RecoveryHandler handler(mem_.get(), &last_sequence);
  for (const uint64_t number : wal_numbers) {
    if (auto s = WalReader::Replay(env_, path_, WalFileName(path_, number), handler); !s.ok()) {
      return s;
    }
  }
  versions_->set_last_sequence(last_sequence);

  if (wal_numbers.empty()) {
    const uint64_t number = versions_->NewFileNumber();
    std::unique_ptr<WritableFile> file;
    if (auto s = env_.NewWritableFile(WalFileName(path_, number), &file); !s.ok()) return s;
    wal_ = std::make_unique<WalWriter>(std::move(file));
    if (auto s = WriteSequenceWatermark(*wal_); !s.ok()) return s;
    mem_wal_numbers_ = {number};
  } else {
    // Continue appending to the newest surviving log. All replayed logs hold
    // part of the current memtable, so none may be deleted until it is flushed.
    std::unique_ptr<WritableFile> file;
    if (auto s = env_.NewAppendableFile(WalFileName(path_, wal_numbers.back()), &file); !s.ok()) {
      return s;
    }
    wal_ = std::make_unique<WalWriter>(std::move(file));
    mem_wal_numbers_ = wal_numbers;
  }

  // The MANIFEST now knows which log is the oldest one still needed, so a later
  // recovery does not replay logs that were already folded into tables.
  VersionEdit edit;
  edit.SetLogNumber(mem_wal_numbers_.front());
  versions_->set_log_number(mem_wal_numbers_.front());
  if (auto s = versions_->LogAndApply(&edit); !s.ok()) return s;

  if (auto s = env_.SyncDir(path_); !s.ok()) return s;

  {
    std::unique_lock<std::mutex> lock(mutex_);
    RemoveObsoleteFiles(lock);
  }

  background_thread_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  while (true) {
    if (!background_error_.ok()) return background_error_;

    if (mem_->ApproximateMemoryUsage() < options_.memtable_bytes) return Status::OK();

    if (imm_ != nullptr) {
      // A previous flush is still running. Waiting here rather than rotating
      // again is what bounds memory: at most two memtables exist at once.
      background_done_.wait(lock);
      continue;
    }

    // Rotate. The new log must exist and be durable before the old memtable is
    // handed over, because from this moment writes go to the new log while the
    // old one is still the only durable copy of the old memtable.
    const uint64_t number = versions_->NewFileNumber();
    std::unique_ptr<WritableFile> file;
    if (auto s = env_.NewWritableFile(WalFileName(path_, number), &file); !s.ok()) return s;
    auto rotated = std::make_unique<WalWriter>(std::move(file));
    if (auto s = WriteSequenceWatermark(*rotated); !s.ok()) return s;
    if (auto s = env_.SyncDir(path_); !s.ok()) return s;

    if (auto s = wal_->Sync(); !s.ok()) return s;
    if (auto s = wal_->Close(); !s.ok()) return s;

    imm_ = mem_;
    imm_wal_numbers_ = mem_wal_numbers_;
    mem_ = std::make_shared<MemTable>();
    mem_wal_numbers_ = {number};
    wal_ = std::move(rotated);

    background_work_.notify_one();
    return Status::OK();
  }
}

DBImpl::Writer* DBImpl::BuildBatchGroup(std::string* operations, uint32_t* count, bool* sync) {
  Writer* leader = writers_.front();
  operations->assign(leader->batch->Contents());
  *count = leader->batch->Count();
  *sync = leader->sync;
  Writer* last = leader;

  if (!options_.enable_group_commit) return last;

  size_t limit = kGroupCommitMaxBytes;
  if (operations->size() < kGroupCommitSmallBatchBytes) {
    limit = operations->size() + kGroupCommitSmallBatchBytes;
  }

  for (auto it = std::next(writers_.begin()); it != writers_.end(); ++it) {
    Writer* candidate = *it;

    // A follower that asked for a sync cannot be folded into a group that is
    // not going to sync: that would silently give it a weaker guarantee than
    // it requested. Stop the group here instead; it becomes the next leader.
    if (candidate->sync && !*sync) break;

    if (operations->size() + candidate->batch->Contents().size() > limit) break;

    operations->append(candidate->batch->Contents());
    *count += candidate->batch->Count();
    last = candidate;
  }
  return last;
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* batch) {
  if (batch == nullptr) return Status::InvalidArgument("batch", "must not be null");
  if (batch->empty()) return Status::OK();

  // WriteOptions::sync can relax durability for one write; it cannot strengthen
  // it past the database's own policy.
  Writer self;
  self.batch = batch;
  self.sync = options_.sync_policy == SyncPolicy::kSyncEveryWrite && options.sync;

  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) return Status::InvalidArgument("db", "is closed");

  writers_.push_back(&self);
  // Sleep until either a leader has written our batch for us, or we have
  // reached the front and become the leader ourselves.
  self.cv.wait(lock, [this, &self] { return self.done || &self == writers_.front(); });
  if (self.done) return self.status;

  // From here we are the leader. Nobody else writes while we hold the front of
  // the queue, which is also what keeps the single-writer contract the skip
  // list depends on.
  Status status = MakeRoomForWrite(lock);

  std::string operations;
  uint32_t count = 0;
  bool group_sync = self.sync;
  Writer* last = &self;
  uint64_t sequence = 0;

  if (status.ok()) {
    last = BuildBatchGroup(&operations, &count, &group_sync);
    sequence = versions_->last_sequence() + 1;
    if (count > kMaxSequenceNumber - sequence) {
      status = Status::InvalidArgument("db", "sequence number space exhausted");
    }
  }

  if (status.ok()) {
    // Captured under the lock so the unlocked section below touches no shared
    // state: only the leader ever changes these, and right now that is us.
    WalWriter* wal = wal_.get();
    MemTable* memtable = mem_.get();
    const std::string payload = EncodeRecordPayload(sequence, count, operations);

    crash::CrashPoint("write_before_wal");

    // The lock is released across the append and the fsync. That is the whole
    // point: the slow part happens while new writers pile up behind us, and
    // they become the next group. Throughput therefore rises with load.
    lock.unlock();
    status = wal->AddRecord(AsBytes(payload), group_sync);

    crash::CrashPoint("write_after_wal_sync");

    // Only after the record is durable does anything become visible in memory.
    if (status.ok()) {
      MemTableInserter inserter(sequence, memtable);
      status = WriteBatch::IterateContents(operations, count, inserter);
    }
    lock.lock();

    if (status.ok()) versions_->set_last_sequence(sequence + count - 1);
  }

  // Hand the result to everyone whose batch we just wrote, and wake them.
  while (true) {
    Writer* front = writers_.front();
    writers_.pop_front();
    if (front != &self) {
      front->status = status;
      front->done = true;
      front->cv.notify_one();
    }
    if (front == last) break;
  }

  // Whoever is now at the front becomes the next leader.
  if (!writers_.empty()) writers_.front()->cv.notify_one();
  return status;
}

Status DBImpl::Get(const ReadOptions& /*options*/, std::string_view key, std::string* value) {
  if (value == nullptr) return Status::InvalidArgument("value", "must not be null");

  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<const Version> version;
  uint64_t snapshot = 0;
  {
    // A tiny critical section: copy three pointers and a number, register the
    // snapshot, and release. The search must not hold the lock, or it would
    // block every writer for the duration of a disk read.
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) return Status::InvalidArgument("db", "is closed");
    mem = mem_;
    imm = imm_;
    version = versions_->current();
    snapshot = versions_->last_sequence();
    // Registering the snapshot is what stops a concurrent compaction from
    // dropping a version this read still needs. Without it, a write landing
    // after this point plus a compaction completing before the search does
    // could make an existing key read as absent.
    active_read_snapshots_.insert(snapshot);
  }
  const ReadSnapshotGuard release_snapshot(this, snapshot);

  // Newest data first, stopping at the first answer. The copied shared_ptrs
  // keep the memtables and the version's open files alive even if the
  // background thread replaces them while this search runs.
  Status status;
  if (mem->Get(key, snapshot, value, &status)) return status;
  if (imm != nullptr && imm->Get(key, snapshot, value, &status)) return status;

  std::string lookup;
  lookup.reserve(key.size() + kInternalKeyTrailerSize);
  AppendInternalKey(&lookup, key, snapshot, kValueTypeForSeek);

  bool found = false;
  Status version_status = version->Get(lookup, key, value, &found);
  if (found) return version_status;
  if (!version_status.ok()) return version_status;
  return Status::NotFound("key");
}

Status DBImpl::FlushImmutableMemTable() {
  std::shared_ptr<MemTable> victim;
  std::vector<uint64_t> victim_wals;
  uint64_t table_number = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    victim = imm_;
    victim_wals = imm_wal_numbers_;
    table_number = versions_->NewFileNumber();
  }
  if (victim == nullptr) return Status::OK();

  const std::string table_path = TableFileName(path_, table_number);
  std::unique_ptr<WritableFile> file;
  if (auto s = env_.NewWritableFile(table_path, &file); !s.ok()) return s;

  SSTableBuilder builder(std::move(file), options_);
  MemTable::Iterator iterator(victim.get());
  for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
    if (auto s = builder.Add(iterator.key(), iterator.value()); !s.ok()) return s;
  }
  if (auto s = builder.Finish(); !s.ok()) return s;

  crash::CrashPoint("flush_after_sst_write");

  // The contents are durable, but the file's NAME is not until the directory is
  // synced. Dying before this leaves a table nothing references, which recovery
  // deletes as garbage -- harmless, because the log still exists.
  if (auto s = env_.SyncDir(path_); !s.ok()) return s;

  FileMetaData meta;
  meta.number = table_number;
  meta.file_size = builder.FileSize();
  meta.smallest = builder.SmallestKey();
  meta.largest = builder.LargestKey();

  {
    std::unique_lock<std::mutex> lock(mutex_);

    // The new memtable's oldest log becomes the oldest one still needed, which
    // is what makes the flushed memtable's logs obsolete.
    VersionEdit edit;
    if (builder.EntryCount() > 0) edit.AddFile(0, meta);
    const uint64_t new_log_number =
        mem_wal_numbers_.empty() ? versions_->log_number() : mem_wal_numbers_.front();
    edit.SetLogNumber(new_log_number);
    versions_->set_log_number(new_log_number);

    crash::CrashPoint("flush_before_wal_delete");

    // Recorded in the MANIFEST before the logs go. Until this is durable the
    // logs are the only copy of this data.
    if (auto s = versions_->LogAndApply(&edit); !s.ok()) return s;

    imm_.reset();
    imm_wal_numbers_.clear();
    RemoveObsoleteFiles(lock);
  }
  background_done_.notify_all();
  return Status::OK();
}

Status DBImpl::RunCompaction() {
  std::shared_ptr<const Version> version;
  uint64_t smallest_snapshot = 0;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    version = versions_->current();
    smallest_snapshot = SmallestActiveSnapshot();
  }
  if (version->files(0).empty()) return Status::OK();

  VersionEdit edit;
  CompactionStats stats;
  const auto allocate = [this]() {
    const std::lock_guard<std::mutex> guard(mutex_);
    return versions_->NewFileNumber();
  };
  if (auto s = CompactLevel0(env_, path_, options_, *version, smallest_snapshot, allocate, &edit,
                             &stats);
      !s.ok()) {
    return s;
  }

  // Release this compaction's own reference to the version it read before
  // asking for cleanup. Holding a Version is precisely what keeps its files
  // from being deleted, so a compaction that kept its own reference would spare
  // exactly the files it had just made obsolete.
  version.reset();

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (auto s = versions_->LogAndApply(&edit); !s.ok()) return s;
    last_compaction_stats_ = stats;
    RemoveObsoleteFiles(lock);
  }
  return Status::OK();
}

void DBImpl::RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock) {
  versions_->ForgetUnreferencedVersions();
  const std::set<uint64_t> live = versions_->LiveFileNumbers();
  const uint64_t oldest_log = versions_->log_number();
  // The live MANIFEST is the one CURRENT names. Comparing against the next file
  // number instead would delete the MANIFEST that was just written, taking the
  // whole database with it.
  const uint64_t live_manifest = versions_->manifest_number();

  std::vector<std::string> children;
  if (!env_.GetChildren(path_, &children).ok()) return;

  std::vector<std::string> doomed;
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (!ParseFileName(name, &number, &type)) continue;

    bool keep = true;
    switch (type) {
      case FileType::kTable:
        // A table no live version references can never be read again. That
        // includes outputs of a compaction that crashed before its MANIFEST
        // record landed.
        keep = live.count(number) > 0;
        break;
      case FileType::kWal:
        keep = number >= oldest_log;
        break;
      case FileType::kManifest:
        // Older MANIFESTs are superseded; CURRENT names the live one.
        keep = number >= live_manifest;
        break;
    }
    if (!keep) doomed.push_back(path_ + "/" + name);
  }
  if (doomed.empty()) return;

  // Unlinking is I/O, so it happens with the lock released.
  lock.unlock();
  for (const std::string& path : doomed) (void)env_.RemoveFile(path);
  (void)env_.SyncDir(path_);
  lock.lock();
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    background_work_.wait(lock, [this] {
      return shutting_down_ || imm_ != nullptr || compaction_requested_ ||
             versions_->NeedsCompaction();
    });
    // On shutdown a pending memtable is deliberately left unflushed: its log is
    // still on disk, so recovery restores it. Flushing would make Close
    // arbitrarily slow for no gain in durability.
    if (shutting_down_) break;

    background_busy_ = true;
    const bool flush_needed = imm_ != nullptr;
    const bool compaction_needed = compaction_requested_ || versions_->NeedsCompaction();
    compaction_requested_ = false;

    lock.unlock();
    Status status;
    if (flush_needed) status = FlushImmutableMemTable();
    if (status.ok() && compaction_needed) status = RunCompaction();
    lock.lock();

    if (!status.ok()) {
      // Writes stop rather than silently continuing with a memtable that can
      // never be flushed or a level that can never be compacted.
      background_error_ = status;
    }
    background_busy_ = false;
    background_done_.notify_all();
  }
}

Status DBImpl::TEST_WaitForBackgroundWork() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (background_error_.ok() && (imm_ != nullptr || background_busy_ || compaction_requested_ ||
                                    versions_->NeedsCompaction())) {
    background_work_.notify_one();
    background_done_.wait(lock);
  }
  return background_error_;
}

Status DBImpl::TEST_CompactNow() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!background_error_.ok()) return background_error_;
    compaction_requested_ = true;
  }
  background_work_.notify_one();
  return TEST_WaitForBackgroundWork();
}

size_t DBImpl::TEST_FileCount(int level) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return versions_->current()->files(level).size();
}

uint64_t DBImpl::TEST_TotalFileBytes() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return versions_->current()->TotalFileBytes();
}

CompactionStats DBImpl::TEST_LastCompactionStats() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return last_compaction_stats_;
}

Status DBImpl::Close() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) return Status::OK();
    closed_ = true;
    shutting_down_ = true;
  }
  background_work_.notify_all();
  background_done_.notify_all();
  if (background_thread_.joinable()) background_thread_.join();

  const std::lock_guard<std::mutex> guard(mutex_);
  Status status;
  if (wal_ != nullptr) {
    // Sync before closing: anything written with kNoSync is still only in the
    // page cache, and a clean Close should not lose it.
    status = wal_->Sync();
    if (auto s = wal_->Close(); status.ok()) status = s;
    wal_.reset();
  }
  lock_.reset();  // Releasing the FileLock unlocks the LOCK file.
  return status;
}

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  if (db == nullptr) return Status::InvalidArgument("db", "must not be null");
  auto impl = std::make_unique<DBImpl>(options, path);
  if (auto s = impl->Recover(); !s.ok()) return s;
  *db = std::move(impl);
  return Status::OK();
}

}  // namespace stratakv
