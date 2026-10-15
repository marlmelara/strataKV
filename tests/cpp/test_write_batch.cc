#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "internal_key.h"

namespace stratakv {
namespace {

// Records what a batch decoded into, so tests can assert on order.
class RecordingHandler final : public WriteBatch::Handler {
 public:
  Status Put(std::string_view key, std::string_view value) override {
    entries.push_back("put " + std::string(key) + "=" + std::string(value));
    return Status::OK();
  }

  Status Delete(std::string_view key) override {
    entries.push_back("del " + std::string(key));
    return Status::OK();
  }

  std::vector<std::string> entries;
};

TEST(WriteBatch, StartsEmpty) {
  const WriteBatch batch;
  EXPECT_TRUE(batch.empty());
  EXPECT_EQ(batch.Count(), 0U);
  EXPECT_TRUE(batch.Contents().empty());
}

TEST(WriteBatch, CountsAndPreservesOperationOrder) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Delete("b");
  batch.Put("c", "3");

  EXPECT_FALSE(batch.empty());
  EXPECT_EQ(batch.Count(), 3U);

  RecordingHandler handler;
  ASSERT_TRUE(batch.Iterate(handler).ok());
  ASSERT_EQ(handler.entries.size(), 3U);
  EXPECT_EQ(handler.entries[0], "put a=1");
  EXPECT_EQ(handler.entries[1], "del b");
  EXPECT_EQ(handler.entries[2], "put c=3");
}

TEST(WriteBatch, HandlesEmptyAndBinaryKeysAndValues) {
  const std::string binary_key("k\0ey", 4);
  const std::string binary_value("v\0al\xFF", 5);

  WriteBatch batch;
  batch.Put("", "");
  batch.Put(binary_key, binary_value);

  RecordingHandler handler;
  ASSERT_TRUE(batch.Iterate(handler).ok());
  ASSERT_EQ(handler.entries.size(), 2U);
  EXPECT_EQ(handler.entries[0], "put =");
  EXPECT_EQ(handler.entries[1], "put " + binary_key + "=" + binary_value);
}

TEST(WriteBatch, ClearResetsBothBufferAndCount) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Clear();

  EXPECT_TRUE(batch.empty());
  EXPECT_EQ(batch.Count(), 0U);

  RecordingHandler handler;
  ASSERT_TRUE(batch.Iterate(handler).ok());
  EXPECT_TRUE(handler.entries.empty());
}

TEST(WriteBatch, LargeValuesUseVarintLengthsCorrectly) {
  const std::string big_value(5000, 'x');  // Needs a two-byte varint length.
  WriteBatch batch;
  batch.Put("k", big_value);

  RecordingHandler handler;
  ASSERT_TRUE(batch.Iterate(handler).ok());
  ASSERT_EQ(handler.entries.size(), 1U);
  EXPECT_EQ(handler.entries[0], "put k=" + big_value);
}

// IterateContents decodes bytes that came from a file, so every malformed shape
// must be reported rather than half-applied.
TEST(WriteBatch, RejectsACountThatDisagreesWithTheContents) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");

  RecordingHandler handler;
  // Claiming one operation when two are encoded.
  EXPECT_TRUE(WriteBatch::IterateContents(batch.Contents(), 1, handler).IsCorruption());

  RecordingHandler handler2;
  // Claiming three when two are encoded.
  EXPECT_TRUE(WriteBatch::IterateContents(batch.Contents(), 3, handler2).IsCorruption());
}

TEST(WriteBatch, RejectsTruncatedOperations) {
  WriteBatch batch;
  batch.Put("key", "value");
  const std::string full(batch.Contents());

  // Every proper prefix is malformed in some way and must be refused.
  for (size_t length = 1; length < full.size(); ++length) {
    RecordingHandler handler;
    const Status status =
        WriteBatch::IterateContents(std::string_view(full).substr(0, length), 1, handler);
    EXPECT_TRUE(status.IsCorruption()) << "prefix of length " << length << " was accepted";
  }
}

TEST(WriteBatch, RejectsAnUnknownOperationType) {
  std::string contents;
  contents.push_back(static_cast<char>(99));  // Not kTypeValue or kTypeDeletion.
  PutLengthPrefixed(&contents, "key");

  RecordingHandler handler;
  EXPECT_TRUE(WriteBatch::IterateContents(contents, 1, handler).IsCorruption());
}

// A handler that fails mid-batch must stop the iteration and surface the error,
// since during recovery that error means the memtable could not be rebuilt.
TEST(WriteBatch, StopsWhenTheHandlerFails) {
  class FailingHandler final : public WriteBatch::Handler {
   public:
    Status Put(std::string_view, std::string_view) override {
      ++calls;
      return Status::IOError("handler", "deliberate failure");
    }
    Status Delete(std::string_view) override {
      ++calls;
      return Status::OK();
    }
    int calls = 0;
  };

  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");

  FailingHandler handler;
  EXPECT_TRUE(batch.Iterate(handler).IsIOError());
  EXPECT_EQ(handler.calls, 1) << "iteration must stop at the first failure";
}

}  // namespace
}  // namespace stratakv
