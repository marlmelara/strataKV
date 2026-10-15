#include "compaction.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "crash_point.h"
#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "iterator.h"
#include "merging_iterator.h"
#include "sstable_builder.h"
#include "version.h"

namespace stratakv {
namespace {

// Ranges are compared on USER keys: two files overlap when they could hold the
// same user key, regardless of sequence numbers.
bool RangesOverlap(std::string_view a_smallest, std::string_view a_largest,
                   std::string_view b_smallest, std::string_view b_largest) {
  return !(a_largest < b_smallest || b_largest < a_smallest);
}

// Holds the output file currently being written, so the writing logic is in one
// place rather than repeated at every point a file might be finished.
class OutputWriter {
 public:
  OutputWriter(Env& env, std::string directory, const Options& options,
               const std::function<uint64_t()>& allocate_file_number)
      : env_(&env),
        directory_(std::move(directory)),
        options_(options),
        allocate_(&allocate_file_number) {}

  Status Add(std::string_view internal_key, std::string_view value) {
    if (builder_ == nullptr) {
      if (auto s = Start(); !s.ok()) return s;
    }
    if (auto s = builder_->Add(internal_key, value); !s.ok()) return s;
    return Status::OK();
  }

  bool HasOpenFile() const { return builder_ != nullptr; }

  uint64_t CurrentSize() const { return builder_ == nullptr ? 0 : builder_->FileSize(); }

  Status Finish(std::vector<FileMetaData>* outputs) {
    if (builder_ == nullptr) return Status::OK();

    if (auto s = builder_->Finish(); !s.ok()) return s;

    FileMetaData meta;
    meta.number = number_;
    meta.file_size = builder_->FileSize();
    meta.smallest = builder_->SmallestKey();
    meta.largest = builder_->LargestKey();
    outputs->push_back(std::move(meta));

    // The output's name must be durable before the MANIFEST can point at it.
    if (auto s = env_->SyncDir(directory_); !s.ok()) return s;

    builder_.reset();
    return Status::OK();
  }

 private:
  Status Start() {
    number_ = (*allocate_)();
    std::unique_ptr<WritableFile> file;
    if (auto s = env_->NewWritableFile(TableFileName(directory_, number_), &file); !s.ok()) {
      return s;
    }
    builder_ = std::make_unique<SSTableBuilder>(std::move(file), options_);
    return Status::OK();
  }

  Env* env_;
  std::string directory_;
  Options options_;
  const std::function<uint64_t()>* allocate_;
  std::unique_ptr<SSTableBuilder> builder_;
  uint64_t number_ = 0;
};

}  // namespace

Status CompactLevel0(Env& env, const std::string& directory, const Options& options,
                     const Version& version, uint64_t smallest_snapshot,
                     const std::function<uint64_t()>& allocate_file_number, VersionEdit* edit,
                     CompactionStats* stats) {
  const std::vector<LevelFile>& level0 = version.files(0);
  if (level0.empty()) return Status::OK();

  // The user-key range the L0 files span, which decides which L1 files have to
  // take part.
  std::string smallest_user(ExtractUserKey(level0.front().meta.smallest));
  std::string largest_user(ExtractUserKey(level0.front().meta.largest));
  for (const LevelFile& file : level0) {
    smallest_user = std::min(smallest_user, std::string(ExtractUserKey(file.meta.smallest)));
    largest_user = std::max(largest_user, std::string(ExtractUserKey(file.meta.largest)));
  }

  std::vector<const LevelFile*> inputs;
  inputs.reserve(level0.size() + version.files(1).size());
  for (const LevelFile& file : level0) inputs.push_back(&file);

  // Every overlapping L1 file joins the compaction. That is what makes dropping
  // tombstones safe: no older version of any key being compacted can survive
  // in a file left behind.
  for (const LevelFile& file : version.files(1)) {
    if (RangesOverlap(smallest_user, largest_user, ExtractUserKey(file.meta.smallest),
                      ExtractUserKey(file.meta.largest))) {
      inputs.push_back(&file);
    }
  }

  std::vector<std::unique_ptr<Iterator>> iterators;
  iterators.reserve(inputs.size());
  for (const LevelFile* file : inputs) iterators.push_back(file->reader->NewIterator());

  const InternalKeyComparator comparator;
  std::unique_ptr<Iterator> merged = NewMergingIterator(comparator, std::move(iterators));

  OutputWriter output(env, directory, options, allocate_file_number);
  std::vector<FileMetaData> outputs;

  std::string last_user_key;
  bool have_last_user_key = false;
  // The sequence number of the previous entry for the current user key. Starts
  // at the maximum for each new key, which means "nothing newer seen yet".
  uint64_t last_sequence_for_key = kMaxSequenceNumber;

  for (merged->SeekToFirst(); merged->Valid(); merged->Next()) {
    const std::string_view internal_key = merged->key();
    if (internal_key.size() < kInternalKeyTrailerSize) {
      return Status::Corruption(directory, "compaction input has a malformed key");
    }
    const std::string_view user_key = ExtractUserKey(internal_key);
    ++stats->entries_read;

    const bool new_user_key = !have_last_user_key || user_key != last_user_key;
    if (new_user_key) {
      // Split output files only at a user-key boundary, so every version of one
      // key lands in the same file. Otherwise two L1 files would both hold that
      // key and the "L1 files never overlap" invariant -- which the read path's
      // binary search depends on -- would quietly break.
      if (output.HasOpenFile() && output.CurrentSize() >= options.l1_file_bytes) {
        if (auto s = output.Finish(&outputs); !s.ok()) return s;
      }
      last_user_key.assign(user_key);
      have_last_user_key = true;
      last_sequence_for_key = kMaxSequenceNumber;
    }

    const uint64_t sequence = ExtractSequence(internal_key);
    const ValueType type = ExtractValueType(internal_key);

    // A newer version of this key has already been emitted and is visible at
    // every snapshot that could still be read, so this one can never be seen
    // again.
    const bool newer_version_already_visible = last_sequence_for_key <= smallest_snapshot;

    // L1 is the bottom level and every overlapping L1 file is in this
    // compaction, so there is nothing older left for this tombstone to hide.
    // Dropping a tombstone above the bottom level would resurrect an older
    // value.
    const bool droppable_tombstone =
        type == ValueType::kTypeDeletion && sequence <= smallest_snapshot;

    const bool drop = newer_version_already_visible || droppable_tombstone;
    last_sequence_for_key = sequence;

    if (drop) {
      ++stats->entries_dropped;
      continue;
    }
    if (auto s = output.Add(internal_key, merged->value()); !s.ok()) return s;
    ++stats->entries_written;
  }

  if (!merged->status().ok()) return merged->status();
  if (auto s = output.Finish(&outputs); !s.ok()) return s;

  // Every output is written and durable, but the MANIFEST does not mention them
  // yet. A crash here leaves files nothing references, which recovery treats as
  // garbage and deletes.
  crash::CrashPoint("compaction_after_output");

  // Remove the inputs from the levels they came from.
  for (const LevelFile& file : level0) edit->RemoveFile(0, file.meta.number);
  for (const LevelFile* file : inputs) {
    // Anything beyond the L0 files came from L1.
    const bool from_level0 = std::any_of(level0.begin(), level0.end(), [&](const LevelFile& l0) {
      return l0.meta.number == file->meta.number;
    });
    if (!from_level0) edit->RemoveFile(1, file->meta.number);
  }
  for (const FileMetaData& meta : outputs) edit->AddFile(1, meta);

  stats->input_files = inputs.size();
  stats->output_files = outputs.size();
  for (const FileMetaData& meta : outputs) stats->bytes_written += meta.file_size;

  // The MANIFEST records the swap next. A crash before that record is durable
  // leaves the old files in place and the new ones unreferenced.
  crash::CrashPoint("compaction_before_manifest");
  return Status::OK();
}

}  // namespace stratakv
