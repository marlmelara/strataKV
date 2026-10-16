#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "env.h"
#include "internal_key.h"
#include "sstable_reader.h"
#include "wal_writer.h"

namespace stratakv {

// Two levels: L0 takes flushed memtables and its files may overlap; L1 is
// produced by compaction and its files never do.
inline constexpr int kNumLevels = 2;

// What the MANIFEST records about one SSTable.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // Smallest internal key in the file.
  std::string largest;   // Largest internal key in the file.
};

// A file plus its open reader. The Version owns the reader, which is what makes
// "a reader holds a Version so its files cannot be deleted underneath it"
// literally true rather than a convention.
struct LevelFile {
  FileMetaData meta;
  std::shared_ptr<SSTableReader> reader;
};

// One change to the set of files, appended to the MANIFEST as a single record.
//
// Recording changes rather than whole states is what makes installing a new
// version an O(1) append instead of a rewrite, and it is why a half-written
// edit can be discarded without losing the edits before it.
class VersionEdit {
 public:
  void SetLogNumber(uint64_t number);
  void SetNextFileNumber(uint64_t number);
  void SetLastSequence(uint64_t sequence);
  void AddFile(int level, const FileMetaData& file);
  void RemoveFile(int level, uint64_t number);

  void EncodeTo(std::string* destination) const;
  Status DecodeFrom(std::string_view source);

  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }
  bool has_next_file_number() const { return has_next_file_number_; }
  uint64_t next_file_number() const { return next_file_number_; }
  bool has_last_sequence() const { return has_last_sequence_; }
  uint64_t last_sequence() const { return last_sequence_; }

  const std::vector<std::pair<int, FileMetaData>>& added_files() const { return added_; }
  const std::vector<std::pair<int, uint64_t>>& removed_files() const { return removed_; }

 private:
  bool has_log_number_ = false;
  uint64_t log_number_ = 0;
  bool has_next_file_number_ = false;
  uint64_t next_file_number_ = 0;
  bool has_last_sequence_ = false;
  uint64_t last_sequence_ = 0;

  std::vector<std::pair<int, FileMetaData>> added_;
  std::vector<std::pair<int, uint64_t>> removed_;
};

// An immutable snapshot of the file set.
//
// Compaction never modifies a Version; it builds a new one. A read copies a
// shared_ptr to the current Version and then searches it with no lock held, so
// the files it needs stay open even while compaction replaces them. When the
// last pointer to an old Version dies, the files only it referenced become
// deletable. This is reference counting used as a concurrency tool.
class Version {
 public:
  Version() = default;

  const std::vector<LevelFile>& files(int level) const {
    return files_[static_cast<size_t>(level)];
  }

  std::vector<LevelFile>& mutable_files(int level) { return files_[static_cast<size_t>(level)]; }

  // Searches L0 newest-first, then the single L1 file whose range covers the
  // key. `*found` separates "no file here holds it" from "a tombstone says it
  // is gone".
  Status Get(std::string_view lookup_key, std::string_view user_key, std::string* value,
             bool* found) const;

  // Total bytes of all files, for the space-amplification check.
  uint64_t TotalFileBytes() const;

  void AddLiveFileNumbers(std::set<uint64_t>* live) const;

 private:
  std::array<std::vector<LevelFile>, kNumLevels> files_;
  InternalKeyComparator comparator_;
};

// Owns the MANIFEST, the current Version, and the counters that have to survive
// a restart: the next file number, the last sequence number, and which log is
// active.
class VersionSet {
 public:
  VersionSet(Env& env, std::string directory, const Options& options);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Reads CURRENT, replays the MANIFEST it names, and opens every file the
  // resulting version references. `*created` is true when there was no database
  // and a fresh MANIFEST was written.
  Status Recover(bool* created);

  // Appends `edit` to the MANIFEST and installs the version it produces.
  //
  // The order is the durability guarantee: the edit is written and synced
  // BEFORE the new version becomes visible, so a crash either leaves the old
  // version intact or a MANIFEST that replays into the new one.
  Status LogAndApply(VersionEdit* edit);

  std::shared_ptr<const Version> current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_++; }
  uint64_t next_file_number() const { return next_file_number_; }
  void MarkFileNumberUsed(uint64_t number);

  uint64_t last_sequence() const { return last_sequence_; }
  void set_last_sequence(uint64_t sequence) { last_sequence_ = sequence; }

  uint64_t log_number() const { return log_number_; }

  // The MANIFEST CURRENT points at. Older ones are superseded and deletable.
  uint64_t manifest_number() const { return manifest_number_; }
  void set_log_number(uint64_t number) { log_number_ = number; }

  // True once L0 has enough files that reads would start to suffer.
  bool NeedsCompaction() const;

  // Every file number any live version still references.
  std::set<uint64_t> LiveFileNumbers() const;

  // Keeps older versions alive while readers hold them, and drops the ones
  // nobody does.
  void ForgetUnreferencedVersions();

 private:
  Status WriteSnapshot();
  Status FinalizeNewVersion(const VersionEdit& edit, std::shared_ptr<Version>* result);

  Env* env_;
  std::string directory_;
  Options options_;
  InternalKeyComparator comparator_;

  std::unique_ptr<WalWriter> manifest_;
  uint64_t manifest_number_ = 0;

  uint64_t next_file_number_ = 1;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;

  std::shared_ptr<const Version> current_;
  // Previous versions that readers may still be holding. Trimmed whenever a new
  // one is installed.
  std::vector<std::shared_ptr<const Version>> retired_;
};

// Points CURRENT at a MANIFEST, atomically.
//
// Four steps, in this order, and every one of them matters:
//   1. write the name into a temporary file
//   2. sync that file, so its contents are on disk
//   3. rename it over CURRENT, which POSIX performs in one step
//   4. sync the directory, so the rename itself survives a crash
//
// Skip step 2 and a crash can leave CURRENT naming nothing, because the rename
// reached disk before the contents did. Skip step 4 and the rename can vanish.
Status SetCurrentFile(Env& env, const std::string& directory, uint64_t manifest_number);

}  // namespace stratakv
