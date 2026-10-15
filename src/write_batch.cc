#include "stratakv/write_batch.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "stratakv/status.h"

#include "internal_key.h"

namespace stratakv {

void WriteBatch::Put(std::string_view key, std::string_view value) {
  rep_.push_back(static_cast<char>(ValueType::kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
  ++count_;
}

void WriteBatch::Delete(std::string_view key) {
  rep_.push_back(static_cast<char>(ValueType::kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
  ++count_;
}

void WriteBatch::Clear() {
  rep_.clear();
  count_ = 0;
}

Status WriteBatch::Iterate(Handler& handler) const {
  return IterateContents(rep_, count_, handler);
}

Status WriteBatch::IterateContents(std::string_view contents, uint32_t count, Handler& handler) {
  uint32_t seen = 0;
  while (!contents.empty()) {
    if (seen == count) {
      return Status::Corruption("write batch", "more operations than the recorded count");
    }
    const auto type = static_cast<ValueType>(contents[0]);
    contents.remove_prefix(1);

    std::string_view key;
    if (!GetLengthPrefixed(&contents, &key)) {
      return Status::Corruption("write batch", "truncated key");
    }

    switch (type) {
      case ValueType::kTypeValue: {
        std::string_view value;
        if (!GetLengthPrefixed(&contents, &value)) {
          return Status::Corruption("write batch", "truncated value");
        }
        if (auto s = handler.Put(key, value); !s.ok()) return s;
        break;
      }
      case ValueType::kTypeDeletion:
        if (auto s = handler.Delete(key); !s.ok()) return s;
        break;
      default:
        return Status::Corruption("write batch", "unknown operation type");
    }
    ++seen;
  }

  if (seen != count) {
    return Status::Corruption("write batch", "fewer operations than the recorded count");
  }
  return Status::OK();
}

}  // namespace stratakv
