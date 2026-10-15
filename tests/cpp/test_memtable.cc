#include <string>

#include <gtest/gtest.h>

#include "stratakv/status.h"

#include "internal_key.h"
#include "memtable.h"

namespace stratakv {
namespace {

TEST(MemTable, EmptyTableAnswersNothing) {
  const MemTable table;
  std::string value;
  Status status;
  EXPECT_FALSE(table.Get("missing", 100, &value, &status))
      << "false means 'keep looking in older data', which an empty table must say";
}

TEST(MemTable, ReturnsTheValueThatWasAdded) {
  MemTable table;
  table.Add(1, ValueType::kTypeValue, "apple", "red");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("apple", 100, &value, &status));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(value, "red");
}

TEST(MemTable, NewestVersionWins) {
  MemTable table;
  table.Add(1, ValueType::kTypeValue, "apple", "first");
  table.Add(2, ValueType::kTypeValue, "apple", "second");
  table.Add(3, ValueType::kTypeValue, "apple", "third");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("apple", 100, &value, &status));
  EXPECT_EQ(value, "third");
}

// A tombstone is an answer, not an absence: it must stop the search rather than
// letting an older value show through.
TEST(MemTable, TombstoneHidesOlderValues) {
  MemTable table;
  table.Add(1, ValueType::kTypeValue, "apple", "red");
  table.Add(2, ValueType::kTypeDeletion, "apple", "");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("apple", 100, &value, &status))
      << "the table knows the answer, so it must return true";
  EXPECT_TRUE(status.IsNotFound());
}

TEST(MemTable, WriteAfterDeleteBringsTheKeyBack) {
  MemTable table;
  table.Add(1, ValueType::kTypeValue, "apple", "red");
  table.Add(2, ValueType::kTypeDeletion, "apple", "");
  table.Add(3, ValueType::kTypeValue, "apple", "green");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("apple", 100, &value, &status));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(value, "green");
}

// A snapshot is a sequence number: a read must not see writes newer than it.
TEST(MemTable, SnapshotHidesNewerWrites) {
  MemTable table;
  table.Add(10, ValueType::kTypeValue, "apple", "ten");
  table.Add(20, ValueType::kTypeValue, "apple", "twenty");

  std::string value;
  Status status;

  ASSERT_TRUE(table.Get("apple", 15, &value, &status));
  EXPECT_EQ(value, "ten") << "sequence 20 is newer than the snapshot and must be invisible";

  ASSERT_TRUE(table.Get("apple", 25, &value, &status));
  EXPECT_EQ(value, "twenty");

  EXPECT_FALSE(table.Get("apple", 5, &value, &status))
      << "every version is newer than this snapshot, so the table says nothing";
}

TEST(MemTable, SnapshotSeesAWriteAtExactlyItsOwnSequence) {
  MemTable table;
  table.Add(10, ValueType::kTypeValue, "apple", "ten");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("apple", 10, &value, &status))
      << "this is what kValueTypeForSeek exists to guarantee";
  EXPECT_EQ(value, "ten");
}

TEST(MemTable, SnapshotSeesATombstoneAtItsOwnSequence) {
  MemTable table;
  table.Add(10, ValueType::kTypeDeletion, "apple", "");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("apple", 10, &value, &status));
  EXPECT_TRUE(status.IsNotFound());
}

TEST(MemTable, DistinguishesKeysWithSharedPrefixes) {
  MemTable table;
  table.Add(1, ValueType::kTypeValue, "app", "short");
  table.Add(2, ValueType::kTypeValue, "apple", "long");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("app", 100, &value, &status));
  EXPECT_EQ(value, "short");
  ASSERT_TRUE(table.Get("apple", 100, &value, &status));
  EXPECT_EQ(value, "long");
  EXPECT_FALSE(table.Get("appl", 100, &value, &status));
}

TEST(MemTable, SupportsEmptyAndBinaryKeysAndValues) {
  const std::string binary_key("k\0ey", 4);
  const std::string binary_value("v\0al", 4);

  MemTable table;
  table.Add(1, ValueType::kTypeValue, "", "empty-key");
  table.Add(2, ValueType::kTypeValue, binary_key, binary_value);
  table.Add(3, ValueType::kTypeValue, "empty-value", "");

  std::string value;
  Status status;
  ASSERT_TRUE(table.Get("", 100, &value, &status));
  EXPECT_EQ(value, "empty-key");
  ASSERT_TRUE(table.Get(binary_key, 100, &value, &status));
  EXPECT_EQ(value, binary_value);
  ASSERT_TRUE(table.Get("empty-value", 100, &value, &status));
  EXPECT_EQ(value, "");
}

TEST(MemTable, MemoryUsageGrowsWithEntries) {
  MemTable table;
  const size_t empty_usage = table.ApproximateMemoryUsage();
  for (int i = 0; i < 2000; ++i) {
    table.Add(static_cast<uint64_t>(i) + 1, ValueType::kTypeValue, "key-" + std::to_string(i),
              std::string(100, 'v'));
  }
  EXPECT_GT(table.ApproximateMemoryUsage(), empty_usage);
}

}  // namespace
}  // namespace stratakv
