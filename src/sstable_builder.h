#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "bloom.h"
#include "env.h"
#include "internal_key.h"

namespace stratakv {

// On-disk layout of an SSTable (a sorted, immutable table of entries):
//
//   [data block 0][data block 1]...[data block N]   ~4 KB each, sorted internal keys
//   [filter block]                                  bloom filter over all user keys
//   [index block]                                   last key of each data block -> (offset, size)
//   [footer, 48 bytes]                              filter (offset,size) | index (offset,size)
//                                                   | magic | crc32c
//
// Data blocks hold entries as `klen | key | vlen | value` with varint lengths.
// Index entries are `last_key | offset(8) | size(4)`, also length-prefixed.
//
// The footer is last and fixed-size so a reader can find everything else with
// one read from the end of the file. Its checksum and magic number are what
// make a half-written table detectable: a flush interrupted before the footer
// lands produces a file that simply does not validate.
inline constexpr size_t kFooterSize = 48;

// "StrataKV" in ASCII, little-endian. Guards against reading a file that is
// not an SSTable at all.
inline constexpr uint64_t kTableMagic = 0x5374726174614B56ULL;

// Builds one SSTable. Keys must arrive in increasing internal-key order, which
// is exactly the order a memtable iterates in.
class SSTableBuilder {
 public:
  SSTableBuilder(std::unique_ptr<WritableFile> file, const Options& options);

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  // Appends one entry. Returns InvalidArgument if the key is out of order,
  // because a table whose entries are not sorted cannot be searched.
  Status Add(std::string_view internal_key, std::string_view value);

  // Writes the pending data block, the filter, the index, and the footer, then
  // forces the whole file to durable storage. The table is only discoverable
  // and trustworthy once this returns OK.
  Status Finish();

  uint64_t EntryCount() const { return entry_count_; }
  uint64_t FileSize() const { return offset_; }

  // Smallest and largest internal keys written. P3's MANIFEST records these to
  // decide which files a compaction has to read.
  const std::string& SmallestKey() const { return smallest_key_; }
  const std::string& LargestKey() const { return largest_key_; }

 private:
  Status FlushDataBlock();
  Status AppendRaw(std::string_view data);

  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  std::unique_ptr<WritableFile> file_;
  size_t block_bytes_;
  InternalKeyComparator comparator_;
  BloomBuilder filter_;

  std::string block_;           // The data block being accumulated.
  std::string block_last_key_;  // Last key in `block_`, for the index entry.
  std::vector<IndexEntry> index_;
  uint64_t offset_ = 0;  // Bytes written so far.
  uint64_t entry_count_ = 0;
  std::string last_key_;  // Last key added, to enforce ordering.
  std::string smallest_key_;
  std::string largest_key_;
  bool finished_ = false;
};

}  // namespace stratakv
