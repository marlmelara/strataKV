#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "stratakv/status.h"

#include "bloom.h"
#include "env.h"
#include "internal_key.h"

namespace stratakv {

// Reads one SSTable. Immutable and thread-safe: positional reads hold no file
// cursor, so any number of threads may search one table at once.
//
// The filter and index are loaded once at Open and kept in memory; a point
// lookup therefore costs at most one disk read, and usually zero when the key
// is absent.
class SSTableReader {
 public:
  // Fails with Corruption if the footer does not validate, which is exactly
  // what a flush interrupted before the footer landed leaves behind.
  static Status Open(Env& env, const std::string& path, std::unique_ptr<SSTableReader>* out);

  SSTableReader(const SSTableReader&) = delete;
  SSTableReader& operator=(const SSTableReader&) = delete;

  // Looks up `user_key`, where `lookup_key` is the internal key to seek to.
  //
  // `*found` separates "this table does not have the key, keep searching older
  // files" from "this table says it is deleted, stop searching". Both would
  // otherwise look like "no value", and conflating them makes deleted keys come
  // back to life.
  Status Get(std::string_view lookup_key, std::string_view user_key, std::string* value,
             bool* found) const;

  size_t BlockCount() const { return index_.size(); }
  const BloomFilter& filter() const { return filter_; }

 private:
  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  SSTableReader(std::unique_ptr<RandomAccessFile> file, std::string path, BloomFilter filter,
                std::vector<IndexEntry> index);

  Status ReadBlock(const IndexEntry& entry, std::string* block) const;

  std::unique_ptr<RandomAccessFile> file_;
  std::string path_;
  BloomFilter filter_;
  std::vector<IndexEntry> index_;
  InternalKeyComparator comparator_;
};

}  // namespace stratakv
