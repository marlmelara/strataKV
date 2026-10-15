#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "stratakv/status.h"

#include "arena.h"
#include "internal_key.h"
#include "skiplist.h"

namespace stratakv {

// The in-memory half of the write path: a sorted table of recent writes.
//
// Writes land here after their WAL record is durable. Readers search it first,
// because it holds the newest versions of everything.
class MemTable {
 public:
  MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Records one operation. The caller must hold the write lock; the sequence
  // number must be larger than every sequence already added.
  void Add(uint64_t sequence, ValueType type, std::string_view key, std::string_view value);

  // Looks up the newest version of `user_key` no newer than `snapshot`.
  //
  // Returns false when this memtable says nothing about the key, so the caller
  // should keep searching older data. Returns true when it has the answer --
  // including when that answer is "deleted", which is reported by setting
  // *status to NotFound. A tombstone is an answer, not an absence.
  bool Get(std::string_view user_key, uint64_t snapshot, std::string* value, Status* status) const;

  // Bytes of arena the table has claimed. Used to decide when to rotate.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

 private:
  InternalKeyComparator comparator_;
  Arena arena_;
  SkipList table_;
};

}  // namespace stratakv
