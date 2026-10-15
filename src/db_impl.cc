#include "db_impl.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
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

#include "crash_point.h"
#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "memtable.h"
#include "sstable_builder.h"
#include "sstable_reader.h"
#include "wal_reader.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

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
    // A zero-operation record is a sequence watermark: it carries no data, but
    // its sequence still tells recovery how far numbering had reached.
    const uint64_t highest = inserter.next_sequence() - 1;
    if (highest > *last_sequence_) *last_sequence_ = highest;
    return Status::OK();
  }

 private:
  MemTable* memtable_;
  uint64_t* last_sequence_;
};

}  // namespace

DBImpl::DBImpl(const Options& options, std::string path)
    : options_(options),
      path_(std::move(path)),
      env_(Env::Default()),
      l0_(std::make_shared<const TableSet>()) {}

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
  // count = 0: no operations, just the sequence number. Recovery derives
  // last_sequence_ = sequence - 1 from it, so the next write continues where
  // this one left off.
  const std::string payload = EncodeRecordPayload(last_sequence_ + 1, 0, {});
  return wal.AddRecord(AsBytes(payload), /*sync=*/true);
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

  // Without a MANIFEST, the directory listing *is* the record of what exists.
  std::vector<std::string> children;
  if (auto s = env_.GetChildren(path_, &children); !s.ok()) return s;

  std::vector<uint64_t> wal_numbers;
  std::vector<uint64_t> table_numbers;
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (!ParseFileName(name, &number, &type)) continue;  // LOCK, or something not ours.
    if (type == FileType::kWal) {
      wal_numbers.push_back(number);
    } else {
      table_numbers.push_back(number);
    }
    if (number >= next_file_number_) next_file_number_ = number + 1;
  }

  std::sort(wal_numbers.begin(), wal_numbers.end());
  std::sort(table_numbers.begin(), table_numbers.end());

  // Open every table, oldest first. A table that fails to validate was being
  // written when the process died: its footer never landed. Discarding it is
  // safe precisely because a log is only deleted after its table is durable,
  // so that data is still in a log waiting to be replayed.
  TableSet tables;
  for (const uint64_t number : table_numbers) {
    const std::string table_path = TableFileName(path_, number);
    std::unique_ptr<SSTableReader> reader;
    Status status = SSTableReader::Open(env_, table_path, &reader);
    if (status.IsCorruption()) {
      if (auto s = env_.RemoveFile(table_path); !s.ok()) return s;
      continue;
    }
    if (!status.ok()) return status;
    tables.push_back(std::move(reader));
  }
  // Newest first: a later table shadows an earlier one for the same key.
  std::reverse(tables.begin(), tables.end());
  l0_ = std::make_shared<const TableSet>(std::move(tables));

  mem_ = std::make_shared<MemTable>();

  // Replay oldest log first so that sequence numbers arrive in order and later
  // versions of a key land on top of earlier ones.
  RecoveryHandler handler(mem_.get(), &last_sequence_);
  for (const uint64_t number : wal_numbers) {
    if (auto s = WalReader::Replay(env_, path_, WalFileName(path_, number), handler); !s.ok()) {
      return s;
    }
  }

  if (wal_numbers.empty()) {
    // Nothing to continue, so start a log and record where sequence numbering
    // had reached -- otherwise a database whose memtables were all flushed
    // would restart numbering from zero and new writes would be ordered before
    // the data already on disk.
    const uint64_t number = next_file_number_++;
    std::unique_ptr<WritableFile> file;
    if (auto s = env_.NewWritableFile(WalFileName(path_, number), &file); !s.ok()) return s;
    wal_ = std::make_unique<WalWriter>(std::move(file));
    if (auto s = WriteSequenceWatermark(*wal_); !s.ok()) return s;
    mem_wal_numbers_ = {number};
  } else {
    // Continue appending to the newest surviving log. Every replayed log holds
    // part of the current memtable, so all of them must stay until it is
    // flushed.
    std::unique_ptr<WritableFile> file;
    if (auto s = env_.NewAppendableFile(WalFileName(path_, wal_numbers.back()), &file); !s.ok()) {
      return s;
    }
    wal_ = std::make_unique<WalWriter>(std::move(file));
    mem_wal_numbers_ = wal_numbers;
  }

  if (auto s = env_.SyncDir(path_); !s.ok()) return s;

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
    // handed over, because from this moment on writes go to the new log while
    // the old one is still the only durable copy of the old memtable.
    const uint64_t number = next_file_number_++;
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

Status DBImpl::Write(const WriteOptions& options, WriteBatch* batch) {
  if (batch == nullptr) return Status::InvalidArgument("batch", "must not be null");
  if (batch->empty()) return Status::OK();

  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) return Status::InvalidArgument("db", "is closed");

  if (auto s = MakeRoomForWrite(lock); !s.ok()) return s;

  const uint64_t sequence = last_sequence_ + 1;
  if (batch->Count() > kMaxSequenceNumber - sequence) {
    return Status::InvalidArgument("db", "sequence number space exhausted");
  }

  const std::string payload = EncodeRecordPayload(sequence, batch->Count(), batch->Contents());

  // WriteOptions::sync can relax durability for one write; it cannot strengthen
  // it past the database's own policy.
  const bool sync = options_.sync_policy == SyncPolicy::kSyncEveryWrite && options.sync;

  crash::CrashPoint("write_before_wal");

  // The order here is the entire durability guarantee. The record reaches the
  // log and is synced BEFORE the memtable is touched and before the caller is
  // told OK. Dying between the sync and the insert is safe: replay rebuilds the
  // memtable from the log. Dying before the sync is also safe: the record was
  // never acknowledged, so truncating it loses nothing that was promised.
  if (auto s = wal_->AddRecord(AsBytes(payload), sync); !s.ok()) return s;

  crash::CrashPoint("write_after_wal_sync");

  MemTableInserter inserter(sequence, mem_.get());
  if (auto s = batch->Iterate(inserter); !s.ok()) return s;

  last_sequence_ = inserter.next_sequence() - 1;
  return Status::OK();
}

Status DBImpl::Get(const ReadOptions& /*options*/, std::string_view key, std::string* value) {
  if (value == nullptr) return Status::InvalidArgument("value", "must not be null");

  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<const TableSet> tables;
  uint64_t snapshot = 0;
  {
    // A tiny critical section: copy three pointers and a number, nothing else.
    // The search must not hold the lock, or it would block every writer for the
    // duration of a disk read.
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) return Status::InvalidArgument("db", "is closed");
    mem = mem_;
    imm = imm_;
    tables = l0_;
    snapshot = last_sequence_;
  }

  // Newest data first, stopping at the first match. The copied shared_ptrs keep
  // the memtables and open files alive even if the background thread replaces
  // them while this search is running.
  Status status;
  if (mem->Get(key, snapshot, value, &status)) return status;
  if (imm != nullptr && imm->Get(key, snapshot, value, &status)) return status;

  std::string lookup;
  lookup.reserve(key.size() + kInternalKeyTrailerSize);
  AppendInternalKey(&lookup, key, snapshot, kValueTypeForSeek);

  for (const std::shared_ptr<SSTableReader>& table : *tables) {
    bool found = false;
    const Status table_status = table->Get(lookup, key, value, &found);
    if (found) return table_status;               // A value, or NotFound for a tombstone.
    if (!table_status.ok()) return table_status;  // A real I/O or corruption error.
  }
  return Status::NotFound("key");
}

Status DBImpl::WriteLevel0Table(const MemTable& table, uint64_t table_number,
                                std::shared_ptr<SSTableReader>* reader) {
  const std::string table_path = TableFileName(path_, table_number);

  std::unique_ptr<WritableFile> file;
  if (auto s = env_.NewWritableFile(table_path, &file); !s.ok()) return s;

  SSTableBuilder builder(std::move(file), options_);
  MemTable::Iterator iterator(&table);
  for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
    if (auto s = builder.Add(iterator.key(), iterator.value()); !s.ok()) return s;
  }
  // Finish writes the filter, index, and footer, then syncs the file. Until it
  // returns OK the table does not exist as far as the engine is concerned.
  if (auto s = builder.Finish(); !s.ok()) return s;

  crash::CrashPoint("flush_after_sst_write");

  // The file's contents are durable, but its *name* is not until the directory
  // is synced. Dying before this leaves a table that recovery may not see --
  // which is harmless, because the log that produced it still exists.
  if (auto s = env_.SyncDir(path_); !s.ok()) return s;

  std::unique_ptr<SSTableReader> opened;
  if (auto s = SSTableReader::Open(env_, table_path, &opened); !s.ok()) return s;
  *reader = std::move(opened);
  return Status::OK();
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    background_work_.wait(lock, [this] { return shutting_down_ || imm_ != nullptr; });
    // On shutdown the pending memtable is deliberately left unflushed: its log
    // is still on disk, so recovery restores it. Flushing here would make Close
    // arbitrarily slow for no gain in durability.
    if (shutting_down_) break;

    const std::shared_ptr<MemTable> victim = imm_;
    const std::vector<uint64_t> victim_wals = imm_wal_numbers_;
    const uint64_t table_number = next_file_number_++;
    flush_in_progress_ = true;

    lock.unlock();
    std::shared_ptr<SSTableReader> reader;
    Status status = WriteLevel0Table(*victim, table_number, &reader);
    lock.lock();

    if (status.ok()) {
      // Install the new table ahead of the others: it is the newest, so it
      // shadows them. Copy-on-write, so a reader already searching the old set
      // is unaffected.
      auto updated = std::make_shared<TableSet>();
      updated->reserve(l0_->size() + 1);
      updated->push_back(std::move(reader));
      for (const std::shared_ptr<SSTableReader>& existing : *l0_) {
        updated->push_back(existing);
      }
      l0_ = std::move(updated);

      crash::CrashPoint("flush_before_wal_delete");

      // Only now may the logs go. Until the table was durable they were the
      // only copy of this data; delete them any earlier and a crash in between
      // loses everything the memtable held.
      lock.unlock();
      for (const uint64_t number : victim_wals) {
        const Status removed = env_.RemoveFile(WalFileName(path_, number));
        if (!removed.ok() && status.ok()) status = removed;
      }
      if (status.ok()) status = env_.SyncDir(path_);
      lock.lock();
    }

    if (status.ok()) {
      imm_.reset();
      imm_wal_numbers_.clear();
    } else {
      // Writes stop rather than silently continuing with a memtable that can
      // never be flushed.
      background_error_ = status;
    }
    flush_in_progress_ = false;
    background_done_.notify_all();
  }
}

Status DBImpl::TEST_WaitForFlush() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (background_error_.ok() && (imm_ != nullptr || flush_in_progress_)) {
    background_done_.wait(lock);
  }
  return background_error_;
}

size_t DBImpl::TEST_Level0FileCount() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return l0_->size();
}

Status DBImpl::Close() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) return Status::OK();
    closed_ = true;
    shutting_down_ = true;
  }
  // Wake the flush thread so it can notice the shutdown, then join it before
  // touching anything it might still be using.
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
