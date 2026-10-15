#pragma once

#include <string_view>

#include "stratakv/status.h"

namespace stratakv {

// A forward cursor over sorted entries.
//
// Compaction merges several of these at once, and they may come from different
// places -- an SSTable on disk, a memtable in memory -- so the merge only needs
// to know this interface.
class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;

  // Valid only while the iterator stays where it is: Next() may invalidate the
  // storage these point into, so a caller that needs to keep a key must copy it.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  // Non-OK once something has gone wrong; the iterator then stops being Valid.
  virtual Status status() const = 0;
};

}  // namespace stratakv
