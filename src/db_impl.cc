#include "db_impl.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "crash_point.h"
#include "env.h"
#include "internal_key.h"
#include "memtable.h"
#include "wal_reader.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

// P1 has no MANIFEST, so the log always has file number 1. P3 assigns numbers
// from the version set.
constexpr uint64_t kInitialWalNumber = 1;

std::string LockFileName(const std::string& directory) { return directory + "/LOCK"; }

std::string WalFileName(const std::string& directory, uint64_t number) {
  // Zero-padded to six digits so the files sort the way they were created.
  const std::string number_text = std::to_string(number);
  return directory + "/" + std::string(6 - std::min<size_t>(6, number_text.size()), '0') +
         number_text + ".log";
}

std::span<const uint8_t> AsBytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
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

// Rebuilds the memtable from WAL records during recovery.
class RecoveryHandler final : public WalRecordHandler {
 public:
  RecoveryHandler(MemTable* memtable, uint64_t* last_sequence)
      : memtable_(memtable), last_sequence_(last_sequence) {}

  Status OnBatch(uint64_t sequence, uint32_t count, std::string_view ops) override {
    MemTableInserter inserter(sequence, memtable_);
    if (auto s = WriteBatch::IterateContents(ops, count, inserter); !s.ok()) return s;
    // next_sequence() is one past the last operation applied.
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
  // instead of corrupting the first one's log.
  if (auto s = env_.LockFile(LockFileName(path_), &lock_); !s.ok()) return s;

  mem_ = std::make_shared<MemTable>();

  const std::string wal_path = WalFileName(path_, kInitialWalNumber);
  RecoveryHandler handler(mem_.get(), &last_sequence_);
  if (auto s = WalReader::Replay(env_, path_, wal_path, handler); !s.ok()) return s;

  // Reopened for appending rather than recreated: in P1 the WAL is the only
  // durable copy of the data, so replacing it would throw everything away.
  std::unique_ptr<WritableFile> file;
  if (auto s = env_.NewAppendableFile(wal_path, &file); !s.ok()) return s;
  wal_ = std::make_unique<WalWriter>(std::move(file));

  // A brand-new directory needs its own entry on disk, or a crash could lose
  // the log file's existence.
  if (!existed) {
    if (auto s = env_.SyncDir(path_); !s.ok()) return s;
  }
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

Status DBImpl::Write(const WriteOptions& options, WriteBatch* batch) {
  if (batch == nullptr) return Status::InvalidArgument("batch", "must not be null");
  if (batch->empty()) return Status::OK();

  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) return Status::InvalidArgument("db", "is closed");

  const uint64_t sequence = last_sequence_ + 1;
  if (batch->Count() > kMaxSequenceNumber - sequence) {
    return Status::InvalidArgument("db", "sequence number space exhausted");
  }

  // payload = seq(8) | count(4) | operations
  std::string payload;
  payload.reserve(12 + batch->Contents().size());
  PutFixed64(&payload, sequence);
  PutFixed32(&payload, batch->Count());
  payload.append(batch->Contents());

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

  std::shared_ptr<MemTable> memtable;
  uint64_t snapshot = 0;
  {
    // A tiny critical section: copy a pointer and a number, nothing else. The
    // search itself must not hold the lock, or it would block every writer.
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) return Status::InvalidArgument("db", "is closed");
    memtable = mem_;
    snapshot = last_sequence_;
  }

  // Searched with no lock held. Safe because the skip list supports any number
  // of lock-free readers alongside the single writer, and because the
  // shared_ptr keeps this memtable alive even once a later phase rotates it.
  Status status;
  if (memtable->Get(key, snapshot, value, &status)) return status;
  return Status::NotFound("key");
}

Status DBImpl::Close() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) return Status::OK();
  closed_ = true;

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
