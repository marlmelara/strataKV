#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "stratakv/status.h"

// Env is the engine's whole view of the operating system: opening files,
// appending, fsync, rename, directory fsync, and the LOCK file. Keeping every
// syscall behind this one interface means the durability rules live in one
// place, and a later phase can substitute a fake Env for testing.
namespace stratakv {

// A file opened for appending. One writer at a time; the engine never appends
// to a file from two threads.
class WritableFile {
 public:
  WritableFile() = default;
  virtual ~WritableFile() = default;

  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  // Appends all of `data`. A short write from the OS is retried internally, so
  // a successful return means every byte reached the kernel -- not the disk.
  virtual Status Append(std::span<const uint8_t> data) = 0;

  // Forces everything appended so far all the way to durable storage. Only
  // after this returns OK may a write be acknowledged to the caller.
  virtual Status Sync() = 0;

  // Cuts the file to `size` bytes. WAL recovery uses this to drop a torn tail.
  virtual Status Truncate(uint64_t size) = 0;

  virtual Status Close() = 0;

  // Bytes appended so far, tracked in memory rather than by stat().
  virtual uint64_t Size() const = 0;

  // Convenience wrapper so callers with text do not have to cast. Not virtual:
  // it forwards to the one Append that subclasses implement.
  Status Append(std::string_view data) {
    return Append(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
  }
};

// A file opened for positional reads. Used by WAL replay and, from P2, by
// SSTable lookups. Reads are thread-safe and stateless: there is no file
// cursor, so several threads may read one file at once.
class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  virtual ~RandomAccessFile() = default;

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // Reads up to `n` bytes at `offset` into `dst`, setting *got to how many
  // arrived. A successful call with *got < n means the file ended early, which
  // for the WAL is the normal shape of a crash rather than an error.
  virtual Status ReadAt(uint64_t offset, uint8_t* dst, size_t n, size_t* got) const = 0;

  virtual uint64_t Size() const = 0;
};

// Holds the database's LOCK file. Releasing is tied to the object's lifetime
// (RAII), so a lock cannot be leaked by an early return: destroying the
// FileLock unlocks and closes the file.
class FileLock {
 public:
  FileLock() = default;
  virtual ~FileLock() = default;

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};

class Env {
 public:
  // Test-only fault injection.
  //
  // The flush and compaction failure paths set a sticky error and stop
  // accepting writes. Reaching them needs an I/O failure on demand, which this
  // provides: the next `count` operations whose path contains `substring` fail
  // with an IOError. Passing an empty substring disarms it.
  //
  // Lives on Env because that is the only place the engine touches the
  // operating system, so one hook covers every path that can fail.
  static void SetFailureInjectionForTesting(std::string_view path_substring, int count);

  // Test-only count of durable syncs performed.
  //
  // This is how group commit is verified in a way that does not depend on the
  // machine: the mechanism's claim is that it issues FEWER syncs for the same
  // work, which is a property of the algorithm. How much time that saves is a
  // property of the disk, and on a virtualized one where fsync does not really
  // flush, it saves almost nothing.
  static uint64_t SyncCountForTesting();
  static void ResetSyncCountForTesting();

  // The process-wide POSIX implementation. Returned by reference because there
  // is exactly one and nobody owns it.
  static Env& Default();

  Env() = default;
  virtual ~Env() = default;

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Creates `path`, truncating it if it already exists.
  virtual Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;

  // Opens `path` for appending, keeping whatever it already holds, and creating
  // it if missing. This is how a recovered WAL is reopened: in P1 the WAL is
  // the only durable copy of the data, so it must be continued rather than
  // replaced.
  virtual Status NewAppendableFile(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;

  virtual Status NewRandomAccessFile(const std::string& path,
                                     std::unique_ptr<RandomAccessFile>* out) = 0;

  // Writes `contents` as the entire file. Does not sync; callers that need
  // durability follow this with SyncFile and, for a new name, SyncDir.
  virtual Status WriteFile(const std::string& path, std::string_view contents) = 0;

  virtual Status ReadFileToString(const std::string& path, std::string* out) = 0;

  // Flushes an already-closed file's contents to durable storage.
  virtual Status SyncFile(const std::string& path) = 0;

  // Flushes a directory's entries, which is what makes a create, rename, or
  // delete inside it survive a crash. The rename itself is not durable until
  // this returns.
  virtual Status SyncDir(const std::string& dir) = 0;

  // Atomically replaces `to` with `from`. A reader sees the old file or the new
  // one, never a mix.
  virtual Status Rename(const std::string& from, const std::string& to) = 0;

  virtual Status RemoveFile(const std::string& path) = 0;

  // Shortens an existing file in place, keeping the first `size` bytes. WAL
  // recovery uses this to drop a torn tail without rewriting the file.
  virtual Status TruncateFile(const std::string& path, uint64_t size) = 0;

  virtual Status CreateDirIfMissing(const std::string& path) = 0;

  virtual bool FileExists(const std::string& path) = 0;

  // Names of the entries in `directory`, excluding "." and "..". Recovery uses
  // this to find the database's files, since P2 has no MANIFEST listing them.
  virtual Status GetChildren(const std::string& directory, std::vector<std::string>* result) = 0;

  virtual Status GetFileSize(const std::string& path, uint64_t* size) = 0;

  // Takes an exclusive, non-blocking lock so two processes cannot open the same
  // database. Fails rather than waiting if the lock is already held.
  virtual Status LockFile(const std::string& path, std::unique_ptr<FileLock>* out) = 0;
};

}  // namespace stratakv
