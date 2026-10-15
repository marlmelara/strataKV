#include "sstable_reader.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stratakv/status.h"

#include "bloom.h"
#include "crc32c.h"
#include "env.h"
#include "internal_key.h"
#include "sstable_builder.h"

namespace stratakv {
namespace {

// The footer's checksum covers everything before the checksum itself.
constexpr size_t kFooterChecksumPrefix = 40;

Status ReadExactly(const RandomAccessFile& file, uint64_t offset, size_t length,
                   const std::string& path, std::string* out) {
  out->assign(length, '\0');
  size_t got = 0;
  if (auto s = file.ReadAt(offset, reinterpret_cast<uint8_t*>(out->data()), length, &got);
      !s.ok()) {
    return s;
  }
  if (got < length) return Status::Corruption(path, "file ended before the expected bytes");
  return Status::OK();
}

}  // namespace

// Walks the table block by block, in index order.
//
// One block is held in memory at a time, so a full scan costs one block of
// memory rather than the whole file -- which matters because compaction opens
// several tables at once.
class TableIterator final : public Iterator {
 public:
  explicit TableIterator(const SSTableReader* reader) : reader_(reader) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    block_index_ = 0;
    valid_ = false;
    status_ = Status::OK();
    LoadBlock();
  }

  void Next() override {
    if (!valid_) return;
    if (!DecodeNextEntry()) {
      // This block is exhausted; move to the next one.
      ++block_index_;
      LoadBlock();
    }
  }

  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

 private:
  // Loads blocks from block_index_ onward until one yields an entry, or the
  // table runs out. Empty blocks cannot occur, but skipping them costs nothing
  // and avoids a sharp edge.
  void LoadBlock() {
    while (block_index_ < reader_->index_.size()) {
      if (auto s = reader_->ReadBlock(reader_->index_[block_index_], &block_); !s.ok()) {
        status_ = s;
        valid_ = false;
        return;
      }
      cursor_ = block_;
      if (DecodeNextEntry()) return;
      ++block_index_;
    }
    valid_ = false;
  }

  // Pulls one entry out of the current block, returning false when it is spent.
  bool DecodeNextEntry() {
    if (cursor_.empty()) {
      valid_ = false;
      return false;
    }
    std::string_view key;
    std::string_view value;
    if (!GetLengthPrefixed(&cursor_, &key) || !GetLengthPrefixed(&cursor_, &value)) {
      status_ = Status::Corruption(reader_->path_, "truncated data block entry");
      valid_ = false;
      return false;
    }
    key_ = key;
    value_ = value;
    valid_ = true;
    return true;
  }

  const SSTableReader* reader_;
  std::string block_;        // Owns the bytes key_ and value_ point into.
  std::string_view cursor_;  // Unread remainder of block_.
  std::string_view key_;
  std::string_view value_;
  size_t block_index_ = 0;
  bool valid_ = false;
  Status status_;
};

std::unique_ptr<Iterator> SSTableReader::NewIterator() const {
  return std::make_unique<TableIterator>(this);
}

SSTableReader::SSTableReader(std::unique_ptr<RandomAccessFile> file, std::string path,
                             BloomFilter filter, std::vector<IndexEntry> index)
    : file_(std::move(file)),
      path_(std::move(path)),
      filter_(std::move(filter)),
      index_(std::move(index)) {}

Status SSTableReader::Open(Env& env, const std::string& path, std::unique_ptr<SSTableReader>* out) {
  std::unique_ptr<RandomAccessFile> file;
  if (auto s = env.NewRandomAccessFile(path, &file); !s.ok()) return s;

  const uint64_t size = file->Size();
  if (size < kFooterSize) {
    return Status::Corruption(path, "file is smaller than a footer");
  }

  // The footer is fixed-size and last, so one read from the end locates
  // everything else.
  std::string footer;
  if (auto s = ReadExactly(*file, size - kFooterSize, kFooterSize, path, &footer); !s.ok()) {
    return s;
  }

  const auto* bytes = reinterpret_cast<const uint8_t*>(footer.data());
  const uint64_t filter_offset = DecodeFixed64(bytes);
  const uint64_t filter_size = DecodeFixed64(bytes + 8);
  const uint64_t index_offset = DecodeFixed64(bytes + 16);
  const uint64_t index_size = DecodeFixed64(bytes + 24);
  const uint64_t magic = DecodeFixed64(bytes + 32);
  const uint32_t stored_crc = DecodeFixed32(bytes + kFooterChecksumPrefix);

  // Magic first: it gives a clearer error for a file that is not an SSTable at
  // all than a checksum mismatch would.
  if (magic != kTableMagic) {
    return Status::Corruption(path, "bad magic number: not an SSTable, or truncated");
  }
  if (crc32c::Value(std::string_view(footer).substr(0, kFooterChecksumPrefix)) != stored_crc) {
    return Status::Corruption(path, "footer checksum mismatch");
  }

  // Offsets come from the file being read, so they are untrusted input.
  const uint64_t metadata_limit = size - kFooterSize;
  if (filter_offset > metadata_limit || filter_size > metadata_limit - filter_offset ||
      index_offset > metadata_limit || index_size > metadata_limit - index_offset) {
    return Status::Corruption(path, "footer points outside the file");
  }

  std::string filter_block;
  if (auto s =
          ReadExactly(*file, filter_offset, static_cast<size_t>(filter_size), path, &filter_block);
      !s.ok()) {
    return s;
  }

  std::string index_block;
  if (auto s =
          ReadExactly(*file, index_offset, static_cast<size_t>(index_size), path, &index_block);
      !s.ok()) {
    return s;
  }

  std::vector<IndexEntry> index;
  std::string_view cursor = index_block;
  while (!cursor.empty()) {
    std::string_view last_key;
    if (!GetLengthPrefixed(&cursor, &last_key)) {
      return Status::Corruption(path, "truncated index entry");
    }
    if (cursor.size() < 12) return Status::Corruption(path, "truncated index entry");
    const auto* entry_bytes = reinterpret_cast<const uint8_t*>(cursor.data());
    IndexEntry entry;
    entry.last_key.assign(last_key);
    entry.offset = DecodeFixed64(entry_bytes);
    entry.size = DecodeFixed32(entry_bytes + 8);
    cursor.remove_prefix(12);

    if (entry.offset > metadata_limit || entry.size > metadata_limit - entry.offset) {
      return Status::Corruption(path, "index entry points outside the data region");
    }
    index.push_back(std::move(entry));
  }

  out->reset(new SSTableReader(std::move(file), path, BloomFilter(filter_block), std::move(index)));
  return Status::OK();
}

Status SSTableReader::ReadBlock(const IndexEntry& entry, std::string* block) const {
  return ReadExactly(*file_, entry.offset, entry.size, path_, block);
}

Status SSTableReader::Get(std::string_view lookup_key, std::string_view user_key,
                          std::string* value, bool* found) const {
  *found = false;

  // For most absent keys this returns without touching the disk at all, which
  // is the entire reason the filter exists.
  if (!filter_.MayContain(user_key)) return Status::OK();

  // Binary search for the first block whose last key is not less than the
  // lookup key: the only block that could hold it.
  const auto block = std::lower_bound(index_.begin(), index_.end(), lookup_key,
                                      [this](const IndexEntry& entry, std::string_view key) {
                                        return comparator_.Compare(entry.last_key, key) < 0;
                                      });
  if (block == index_.end()) return Status::OK();  // Larger than everything here.

  std::string contents;
  if (auto s = ReadBlock(*block, &contents); !s.ok()) return s;  // The one disk read.

  std::string_view cursor = contents;
  while (!cursor.empty()) {
    std::string_view key;
    std::string_view entry_value;
    if (!GetLengthPrefixed(&cursor, &key) || !GetLengthPrefixed(&cursor, &entry_value)) {
      return Status::Corruption(path_, "truncated data block entry");
    }
    if (key.size() < kInternalKeyTrailerSize) {
      return Status::Corruption(path_, "data block entry key is too short");
    }

    if (comparator_.Compare(key, lookup_key) >= 0) {
      // Reached where the key would be. If the entry here belongs to a
      // different user key, this table simply does not hold it.
      if (ExtractUserKey(key) != user_key) return Status::OK();

      *found = true;
      if (ExtractValueType(key) == ValueType::kTypeDeletion) {
        // A tombstone is an answer: stop, do not search older files.
        return Status::NotFound("key");
      }
      value->assign(entry_value);
      return Status::OK();
    }
  }
  return Status::OK();
}

}  // namespace stratakv
