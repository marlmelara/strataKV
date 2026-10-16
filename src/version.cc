#include "version.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stratakv/status.h"

#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "sstable_reader.h"
#include "wal_reader.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

// Field tags in an encoded VersionEdit. Tagged fields mean a later phase can
// add a field without making older MANIFESTs unreadable.
enum class EditTag : uint8_t {
  kLogNumber = 1,
  kNextFileNumber = 2,
  kLastSequence = 3,
  kRemovedFile = 4,
  kAddedFile = 5,
};

void PutTag(std::string* destination, EditTag tag) {
  PutVarint32(destination, static_cast<uint32_t>(tag));
}

std::span<const uint8_t> AsBytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

bool ValidLevel(uint32_t level) { return level < static_cast<uint32_t>(kNumLevels); }

}  // namespace

// --- VersionEdit ---

void VersionEdit::SetLogNumber(uint64_t number) {
  has_log_number_ = true;
  log_number_ = number;
}

void VersionEdit::SetNextFileNumber(uint64_t number) {
  has_next_file_number_ = true;
  next_file_number_ = number;
}

void VersionEdit::SetLastSequence(uint64_t sequence) {
  has_last_sequence_ = true;
  last_sequence_ = sequence;
}

void VersionEdit::AddFile(int level, const FileMetaData& file) { added_.emplace_back(level, file); }

void VersionEdit::RemoveFile(int level, uint64_t number) { removed_.emplace_back(level, number); }

void VersionEdit::EncodeTo(std::string* destination) const {
  if (has_log_number_) {
    PutTag(destination, EditTag::kLogNumber);
    PutFixed64(destination, log_number_);
  }
  if (has_next_file_number_) {
    PutTag(destination, EditTag::kNextFileNumber);
    PutFixed64(destination, next_file_number_);
  }
  if (has_last_sequence_) {
    PutTag(destination, EditTag::kLastSequence);
    PutFixed64(destination, last_sequence_);
  }
  for (const auto& [level, number] : removed_) {
    PutTag(destination, EditTag::kRemovedFile);
    PutVarint32(destination, static_cast<uint32_t>(level));
    PutFixed64(destination, number);
  }
  for (const auto& [level, file] : added_) {
    PutTag(destination, EditTag::kAddedFile);
    PutVarint32(destination, static_cast<uint32_t>(level));
    PutFixed64(destination, file.number);
    PutFixed64(destination, file.file_size);
    PutLengthPrefixed(destination, file.smallest);
    PutLengthPrefixed(destination, file.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view source) {
  // Every field here came off disk, so each decode step is checked.
  const auto corrupt = [](std::string_view what) {
    return Status::Corruption("version edit", what);
  };

  while (!source.empty()) {
    uint32_t tag = 0;
    if (!GetVarint32(&source, &tag)) return corrupt("truncated tag");

    switch (static_cast<EditTag>(tag)) {
      case EditTag::kLogNumber: {
        if (source.size() < 8) return corrupt("truncated log number");
        SetLogNumber(DecodeFixed64(reinterpret_cast<const uint8_t*>(source.data())));
        source.remove_prefix(8);
        break;
      }
      case EditTag::kNextFileNumber: {
        if (source.size() < 8) return corrupt("truncated next file number");
        SetNextFileNumber(DecodeFixed64(reinterpret_cast<const uint8_t*>(source.data())));
        source.remove_prefix(8);
        break;
      }
      case EditTag::kLastSequence: {
        if (source.size() < 8) return corrupt("truncated last sequence");
        SetLastSequence(DecodeFixed64(reinterpret_cast<const uint8_t*>(source.data())));
        source.remove_prefix(8);
        break;
      }
      case EditTag::kRemovedFile: {
        uint32_t level = 0;
        if (!GetVarint32(&source, &level)) return corrupt("truncated removed-file level");
        if (!ValidLevel(level)) return corrupt("removed-file level out of range");
        if (source.size() < 8) return corrupt("truncated removed-file number");
        RemoveFile(static_cast<int>(level),
                   DecodeFixed64(reinterpret_cast<const uint8_t*>(source.data())));
        source.remove_prefix(8);
        break;
      }
      case EditTag::kAddedFile: {
        uint32_t level = 0;
        if (!GetVarint32(&source, &level)) return corrupt("truncated added-file level");
        if (!ValidLevel(level)) return corrupt("added-file level out of range");
        if (source.size() < 16) return corrupt("truncated added-file numbers");
        FileMetaData file;
        const auto* bytes = reinterpret_cast<const uint8_t*>(source.data());
        file.number = DecodeFixed64(bytes);
        file.file_size = DecodeFixed64(bytes + 8);
        source.remove_prefix(16);

        std::string_view smallest;
        std::string_view largest;
        if (!GetLengthPrefixed(&source, &smallest) || !GetLengthPrefixed(&source, &largest)) {
          return corrupt("truncated added-file key range");
        }
        if (smallest.size() < kInternalKeyTrailerSize || largest.size() < kInternalKeyTrailerSize) {
          return corrupt("added-file key range is not made of internal keys");
        }
        file.smallest.assign(smallest);
        file.largest.assign(largest);
        AddFile(static_cast<int>(level), file);
        break;
      }
      default:
        return corrupt("unknown tag");
    }
  }
  return Status::OK();
}

// --- Version ---

Status Version::Get(std::string_view lookup_key, std::string_view user_key, std::string* value,
                    bool* found) const {
  *found = false;

  // L0 files overlap, so every one of them has to be consulted, newest first.
  for (const LevelFile& file : files_[0]) {
    Status status = file.reader->Get(lookup_key, user_key, value, found);
    if (*found) return status;
    if (!status.ok()) return status;
  }

  // L1 files never overlap, so at most one can hold the key. A binary search
  // finds it: the first file whose largest key is not less than the lookup key.
  const std::vector<LevelFile>& level1 = files_[1];
  const auto candidate = std::lower_bound(level1.begin(), level1.end(), lookup_key,
                                          [this](const LevelFile& file, std::string_view key) {
                                            return comparator_.Compare(file.meta.largest, key) < 0;
                                          });
  if (candidate == level1.end()) return Status::OK();

  // The key may sort before this file's range, in which case no file holds it.
  if (comparator_.Compare(lookup_key, candidate->meta.smallest) < 0) {
    if (ExtractUserKey(candidate->meta.smallest) != user_key) return Status::OK();
  }
  return candidate->reader->Get(lookup_key, user_key, value, found);
}

uint64_t Version::TotalFileBytes() const {
  uint64_t total = 0;
  for (const std::vector<LevelFile>& level : files_) {
    for (const LevelFile& file : level) total += file.meta.file_size;
  }
  return total;
}

void Version::AddLiveFileNumbers(std::set<uint64_t>* live) const {
  for (const std::vector<LevelFile>& level : files_) {
    for (const LevelFile& file : level) live->insert(file.meta.number);
  }
}

// --- VersionSet ---

VersionSet::VersionSet(Env& env, std::string directory, const Options& options)
    : env_(&env),
      directory_(std::move(directory)),
      options_(options),
      current_(std::make_shared<const Version>()) {}

VersionSet::~VersionSet() = default;

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) next_file_number_ = number + 1;
}

bool VersionSet::NeedsCompaction() const {
  return static_cast<int>(current_->files(0).size()) >= options_.l0_compaction_trigger;
}

std::set<uint64_t> VersionSet::LiveFileNumbers() const {
  std::set<uint64_t> live;
  current_->AddLiveFileNumbers(&live);
  // A version a reader is still holding counts as live, even though it is no
  // longer current: its files must not be deleted while that read runs.
  for (const std::shared_ptr<const Version>& version : retired_) {
    version->AddLiveFileNumbers(&live);
  }
  return live;
}

void VersionSet::ForgetUnreferencedVersions() {
  // use_count() == 1 means this vector holds the only pointer, so no reader can
  // still be searching that version.
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [](const std::shared_ptr<const Version>& version) {
                                  return version.use_count() == 1;
                                }),
                 retired_.end());
}

// Builds the version that results from applying `edit` to the current one,
// opening readers for added files and reusing the readers already open for
// files that survive.
Status VersionSet::FinalizeNewVersion(const VersionEdit& edit, std::shared_ptr<Version>* result) {
  auto next = std::make_shared<Version>();

  for (int level = 0; level < kNumLevels; ++level) {
    std::set<uint64_t> removed;
    for (const auto& [removed_level, number] : edit.removed_files()) {
      if (removed_level == level) removed.insert(number);
    }

    for (const LevelFile& file : current_->files(level)) {
      if (removed.count(file.meta.number) == 0) {
        // Reader reused rather than reopened: the file has not changed, and
        // reopening would throw away its cached index and filter.
        next->mutable_files(level).push_back(file);
      }
    }

    for (const auto& [added_level, meta] : edit.added_files()) {
      if (added_level != level) continue;
      LevelFile file;
      file.meta = meta;
      std::unique_ptr<SSTableReader> reader;
      if (auto s = SSTableReader::Open(*env_, TableFileName(directory_, meta.number), &reader);
          !s.ok()) {
        return s;
      }
      file.reader = std::move(reader);
      next->mutable_files(level).push_back(std::move(file));
    }
  }

  // L0 is searched newest-first, so keep it ordered by descending file number.
  std::vector<LevelFile>& level0 = next->mutable_files(0);
  std::sort(level0.begin(), level0.end(),
            [](const LevelFile& a, const LevelFile& b) { return a.meta.number > b.meta.number; });

  // L1 never overlaps, so ordering it by key range is what lets a read binary
  // search it.
  std::vector<LevelFile>& level1 = next->mutable_files(1);
  std::sort(level1.begin(), level1.end(), [this](const LevelFile& a, const LevelFile& b) {
    return comparator_.Compare(a.meta.smallest, b.meta.smallest) < 0;
  });

  *result = std::move(next);
  return Status::OK();
}

Status VersionSet::WriteSnapshot() {
  // A fresh MANIFEST starts with one edit describing the whole current state,
  // so replaying it from the beginning reproduces exactly this version.
  VersionEdit snapshot;
  snapshot.SetNextFileNumber(next_file_number_);
  snapshot.SetLastSequence(last_sequence_);
  snapshot.SetLogNumber(log_number_);
  for (int level = 0; level < kNumLevels; ++level) {
    for (const LevelFile& file : current_->files(level)) {
      snapshot.AddFile(level, file.meta);
    }
  }

  std::string record;
  snapshot.EncodeTo(&record);
  return manifest_->AddRecord(AsBytes(record), /*sync=*/true);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit == nullptr) return Status::InvalidArgument("edit", "must not be null");

  edit->SetNextFileNumber(next_file_number_);
  edit->SetLastSequence(last_sequence_);
  edit->SetLogNumber(log_number_);

  std::shared_ptr<Version> next;
  if (auto s = FinalizeNewVersion(*edit, &next); !s.ok()) return s;

  // Written and synced before the new version becomes visible. A crash here
  // leaves either the old version intact, or a MANIFEST that replays into the
  // new one -- never a version referencing a file the MANIFEST does not list.
  std::string record;
  edit->EncodeTo(&record);
  if (auto s = manifest_->AddRecord(AsBytes(record), /*sync=*/true); !s.ok()) return s;

  retired_.push_back(current_);
  current_ = std::move(next);
  ForgetUnreferencedVersions();
  return Status::OK();
}

namespace {

// Applies every edit in a MANIFEST, accumulating the file set.
class ManifestReplayHandler final : public RecordPayloadHandler {
 public:
  Status OnRecord(std::string_view payload) override {
    VersionEdit edit;
    if (auto s = edit.DecodeFrom(payload); !s.ok()) return s;

    for (const auto& [level, number] : edit.removed_files()) {
      files[static_cast<size_t>(level)].erase(number);
    }
    for (const auto& [level, meta] : edit.added_files()) {
      files[static_cast<size_t>(level)][meta.number] = meta;
    }
    if (edit.has_log_number()) log_number = edit.log_number();
    if (edit.has_next_file_number()) next_file_number = edit.next_file_number();
    if (edit.has_last_sequence()) last_sequence = edit.last_sequence();
    ++edits;
    return Status::OK();
  }

  std::array<std::map<uint64_t, FileMetaData>, kNumLevels> files;
  uint64_t log_number = 0;
  uint64_t next_file_number = 1;
  uint64_t last_sequence = 0;
  int edits = 0;
};

}  // namespace

Status VersionSet::Recover(bool* created) {
  *created = false;
  const std::string current_path = directory_ + "/CURRENT";

  if (!env_->FileExists(current_path)) {
    // A brand-new database: write an empty MANIFEST and point CURRENT at it.
    *created = true;
    manifest_number_ = NewFileNumber();
    std::unique_ptr<WritableFile> file;
    if (auto s = env_->NewWritableFile(ManifestFileName(directory_, manifest_number_), &file);
        !s.ok()) {
      return s;
    }
    manifest_ = std::make_unique<WalWriter>(std::move(file));
    if (auto s = WriteSnapshot(); !s.ok()) return s;
    return SetCurrentFile(*env_, directory_, manifest_number_);
  }

  std::string contents;
  if (auto s = env_->ReadFileToString(current_path, &contents); !s.ok()) return s;
  while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
    contents.pop_back();
  }
  uint64_t number = 0;
  FileType type{};
  if (!ParseFileName(contents, &number, &type) || type != FileType::kManifest) {
    return Status::Corruption(current_path, "does not name a MANIFEST: '" + contents + "'");
  }

  const std::string manifest_path = ManifestFileName(directory_, number);
  ManifestReplayHandler handler;
  if (auto s = WalReader::ReplayRecords(*env_, directory_, manifest_path, handler); !s.ok()) {
    return s;
  }
  if (handler.edits == 0) {
    return Status::Corruption(manifest_path, "MANIFEST contains no usable edits");
  }

  next_file_number_ = handler.next_file_number;
  last_sequence_ = handler.last_sequence;
  log_number_ = handler.log_number;
  MarkFileNumberUsed(number);

  // Build the recovered version, opening every file it references. A file the
  // MANIFEST lists but which is missing or invalid is a genuine inconsistency:
  // unlike P2's directory scan, the MANIFEST is authoritative.
  auto recovered = std::make_shared<Version>();
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& [file_number, meta] : handler.files[static_cast<size_t>(level)]) {
      LevelFile file;
      file.meta = meta;
      std::unique_ptr<SSTableReader> reader;
      if (auto s = SSTableReader::Open(*env_, TableFileName(directory_, file_number), &reader);
          !s.ok()) {
        return s;
      }
      file.reader = std::move(reader);
      recovered->mutable_files(level).push_back(std::move(file));
      MarkFileNumberUsed(file_number);
    }
  }

  std::vector<LevelFile>& level0 = recovered->mutable_files(0);
  std::sort(level0.begin(), level0.end(),
            [](const LevelFile& a, const LevelFile& b) { return a.meta.number > b.meta.number; });
  std::vector<LevelFile>& level1 = recovered->mutable_files(1);
  std::sort(level1.begin(), level1.end(), [this](const LevelFile& a, const LevelFile& b) {
    return comparator_.Compare(a.meta.smallest, b.meta.smallest) < 0;
  });
  current_ = std::move(recovered);

  // Start a new MANIFEST rather than appending to the old one, so a MANIFEST
  // never grows without bound across restarts.
  manifest_number_ = NewFileNumber();
  std::unique_ptr<WritableFile> file;
  if (auto s = env_->NewWritableFile(ManifestFileName(directory_, manifest_number_), &file);
      !s.ok()) {
    return s;
  }
  manifest_ = std::make_unique<WalWriter>(std::move(file));
  if (auto s = WriteSnapshot(); !s.ok()) return s;
  return SetCurrentFile(*env_, directory_, manifest_number_);
}

Status SetCurrentFile(Env& env, const std::string& directory, uint64_t manifest_number) {
  const std::string temporary = directory + "/CURRENT.tmp";
  const std::string name = ManifestFileName(directory, manifest_number);
  const std::string bare = name.substr(name.rfind('/') + 1);

  // 1. Write the new contents to a temporary file, never to CURRENT itself, so
  //    a crash mid-write can only damage the temporary one.
  if (auto s = env.WriteFile(temporary, bare + "\n"); !s.ok()) return s;
  // 2. Force those bytes to disk before the file gets the real name.
  if (auto s = env.SyncFile(temporary); !s.ok()) return s;
  // 3. POSIX rename replaces the target in one step: a reader sees the whole
  //    old file or the whole new one, never a mix.
  if (auto s = env.Rename(temporary, directory + "/CURRENT"); !s.ok()) return s;
  // 4. A file's name lives in its directory, which is its own data on disk.
  return env.SyncDir(directory);
}

}  // namespace stratakv
