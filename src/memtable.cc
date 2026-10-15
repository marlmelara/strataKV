#include "memtable.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "stratakv/status.h"

#include "internal_key.h"
#include "skiplist.h"

namespace stratakv {

MemTable::MemTable() : table_(comparator_, &arena_) {}

void MemTable::Add(uint64_t sequence, ValueType type, std::string_view key,
                   std::string_view value) {
  std::string internal_key;
  internal_key.reserve(key.size() + kInternalKeyTrailerSize);
  AppendInternalKey(&internal_key, key, sequence, type);
  table_.Insert(internal_key, value);
}

bool MemTable::Get(std::string_view user_key, uint64_t snapshot, std::string* value,
                   Status* status) const {
  // Seek to (user_key, snapshot). Because the trailer sorts descending, the
  // first entry found for this user key is the newest one at or below the
  // snapshot; anything newer sorts before the seek target and is skipped.
  std::string lookup;
  lookup.reserve(user_key.size() + kInternalKeyTrailerSize);
  AppendInternalKey(&lookup, user_key, snapshot, kValueTypeForSeek);

  SkipList::Iterator iterator(&table_);
  iterator.Seek(lookup);
  if (!iterator.Valid()) return false;

  const std::string_view found = iterator.key();
  if (ExtractUserKey(found) != user_key) return false;  // Landed on a different key.

  switch (ExtractValueType(found)) {
    case ValueType::kTypeValue:
      value->assign(iterator.value());
      *status = Status::OK();
      return true;
    case ValueType::kTypeDeletion:
      // A tombstone hides every older version, so the search stops here.
      *status = Status::NotFound("key");
      return true;
  }
  return false;
}

}  // namespace stratakv
