#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "env.h"
#include "internal_key.h"
#include "sstable_builder.h"
#include "sstable_reader.h"
#include "test_util.h"

namespace stratakv {
namespace {

using testing::TempDir;

std::string Internal(std::string_view user_key, uint64_t sequence,
                     ValueType type = ValueType::kTypeValue) {
  std::string out;
  AppendInternalKey(&out, user_key, sequence, type);
  return out;
}

std::string Lookup(std::string_view user_key, uint64_t snapshot = kMaxSequenceNumber) {
  std::string out;
  AppendInternalKey(&out, user_key, snapshot, kValueTypeForSeek);
  return out;
}

class SSTableTest : public ::testing::Test {
 protected:
  std::string Path() const { return dir_.Child("000001.sst"); }

  // Builds a table from already-sorted entries.
  void Build(const std::vector<std::pair<std::string, std::string>>& entries,
             Options options = Options{}) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(env_.NewWritableFile(Path(), &file).ok());
    SSTableBuilder builder(std::move(file), options);
    for (const auto& [key, value] : entries) {
      ASSERT_TRUE(builder.Add(key, value).ok()) << key;
    }
    ASSERT_TRUE(builder.Finish().ok());
    entry_count_ = builder.EntryCount();
    file_size_ = builder.FileSize();
  }

  std::unique_ptr<SSTableReader> OpenOrDie() {
    std::unique_ptr<SSTableReader> reader;
    const Status status = SSTableReader::Open(env_, Path(), &reader);
    EXPECT_TRUE(status.ok()) << status.ToString();
    // Abort the test rather than returning null and segfaulting at the first
    // use, which hides whatever actually went wrong.
    if (reader == nullptr) std::abort();
    return reader;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
  uint64_t entry_count_ = 0;
  uint64_t file_size_ = 0;
};

TEST_F(SSTableTest, RoundTripsASingleEntry) {
  Build({{Internal("apple", 1), "red"}});
  auto reader = OpenOrDie();

  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup("apple"), "apple", &value, &found).ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "red");
}

TEST_F(SSTableTest, ReportsAbsentKeysWithoutClaimingDeletion) {
  Build({{Internal("apple", 1), "red"}});
  auto reader = OpenOrDie();

  std::string value;
  bool found = false;
  const Status status = reader->Get(Lookup("banana"), "banana", &value, &found);
  ASSERT_TRUE(status.ok());
  EXPECT_FALSE(found) << "found=false means 'keep searching older files'";
}

TEST_F(SSTableTest, TombstoneIsFoundAndReportsNotFound) {
  Build({{Internal("apple", 2, ValueType::kTypeDeletion), ""}});
  auto reader = OpenOrDie();

  std::string value;
  bool found = false;
  const Status status = reader->Get(Lookup("apple"), "apple", &value, &found);
  EXPECT_TRUE(found) << "a tombstone IS an answer, so the search must stop here";
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(SSTableTest, FindsEveryKeyAcrossManyBlocks) {
  // Small blocks so the table definitely spans many of them, exercising the
  // index binary search rather than a single-block shortcut.
  Options options;
  options.block_bytes = 256;

  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 2000; ++i) {
    entries.emplace_back(Internal(testing::NumberedKey(i), static_cast<uint64_t>(i) + 1),
                         testing::NumberedValue(i));
  }
  Build(entries, options);

  auto reader = OpenOrDie();
  EXPECT_GT(reader->BlockCount(), 10U) << "the table should span many blocks";

  for (int i = 0; i < 2000; ++i) {
    std::string value;
    bool found = false;
    ASSERT_TRUE(
        reader->Get(Lookup(testing::NumberedKey(i)), testing::NumberedKey(i), &value, &found).ok())
        << i;
    ASSERT_TRUE(found) << "missing key " << i;
    EXPECT_EQ(value, testing::NumberedValue(i)) << i;
  }
}

TEST_F(SSTableTest, MissesBetweenAndBeyondStoredKeysAreReportedAsAbsent) {
  Options options;
  options.block_bytes = 256;
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 500; ++i) {
    // Only even keys, so every odd one is a true miss.
    entries.emplace_back(Internal(testing::NumberedKey(i * 2), static_cast<uint64_t>(i) + 1), "v");
  }
  Build(entries, options);
  auto reader = OpenOrDie();

  for (int i = 0; i < 500; ++i) {
    const std::string key = testing::NumberedKey(i * 2 + 1);
    std::string value;
    bool found = false;
    ASSERT_TRUE(reader->Get(Lookup(key), key, &value, &found).ok());
    EXPECT_FALSE(found) << key;
  }

  // Past the end of the table entirely.
  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup("zzzz"), "zzzz", &value, &found).ok());
  EXPECT_FALSE(found);

  // Before the beginning.
  ASSERT_TRUE(reader->Get(Lookup("aaaa"), "aaaa", &value, &found).ok());
  EXPECT_FALSE(found);
}

TEST_F(SSTableTest, NewestVersionOfAKeyIsReturned) {
  // Internal keys sort newest-first within one user key, so the newest version
  // is the first one the block scan reaches.
  Build({{Internal("apple", 5), "newest"},
         {Internal("apple", 3), "middle"},
         {Internal("apple", 1), "oldest"}});
  auto reader = OpenOrDie();

  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup("apple"), "apple", &value, &found).ok());
  ASSERT_TRUE(found);
  EXPECT_EQ(value, "newest");
}

TEST_F(SSTableTest, SnapshotHidesNewerVersions) {
  Build({{Internal("apple", 5), "newest"}, {Internal("apple", 1), "oldest"}});
  auto reader = OpenOrDie();

  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup("apple", 3), "apple", &value, &found).ok());
  ASSERT_TRUE(found);
  EXPECT_EQ(value, "oldest") << "sequence 5 is newer than the snapshot";
}

TEST_F(SSTableTest, HandlesEmptyAndBinaryKeysAndValues) {
  const std::string binary_key("k\0ey", 4);
  // Sorted by user key: "" < "empty-value" < "k\0ey", because 'e' < 'k'.
  const std::vector<std::pair<std::string, std::string>> entries = {
      {Internal("", 1), "empty-key"},
      {Internal("empty-value", 3), ""},
      {Internal(binary_key, 2), std::string("v\0al", 4)},
  };
  Build(entries);
  auto reader = OpenOrDie();

  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup(""), "", &value, &found).ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "empty-key");

  ASSERT_TRUE(reader->Get(Lookup(binary_key), binary_key, &value, &found).ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(value, std::string("v\0al", 4));

  ASSERT_TRUE(reader->Get(Lookup("empty-value"), "empty-value", &value, &found).ok());
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "");
}

TEST_F(SSTableTest, HandlesAValueLargerThanOneBlock) {
  Options options;
  options.block_bytes = 256;
  const std::string big(10000, 'x');
  Build({{Internal("big", 1), big}}, options);

  auto reader = OpenOrDie();
  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup("big"), "big", &value, &found).ok());
  ASSERT_TRUE(found);
  EXPECT_EQ(value, big) << "an entry larger than a block must get a block of its own";
}

TEST_F(SSTableTest, RejectsOutOfOrderKeys) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(Path(), &file).ok());
  SSTableBuilder builder(std::move(file), Options{});

  ASSERT_TRUE(builder.Add(Internal("banana", 1), "b").ok());
  // Out of order: the index binary search and the block scan both assume
  // sorted input, so this must be refused rather than silently accepted.
  EXPECT_TRUE(builder.Add(Internal("apple", 1), "a").IsInvalidArgument());
  // And a duplicate of the same internal key.
  EXPECT_TRUE(builder.Add(Internal("banana", 1), "b2").IsInvalidArgument());
}

TEST_F(SSTableTest, RejectsAKeyTooShortToBeAnInternalKey) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(Path(), &file).ok());
  SSTableBuilder builder(std::move(file), Options{});
  EXPECT_TRUE(builder.Add("short", "v").IsInvalidArgument());
}

TEST_F(SSTableTest, ReportsSmallestAndLargestKeys) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(Path(), &file).ok());
  SSTableBuilder builder(std::move(file), Options{});
  ASSERT_TRUE(builder.Add(Internal("apple", 1), "a").ok());
  ASSERT_TRUE(builder.Add(Internal("banana", 2), "b").ok());
  ASSERT_TRUE(builder.Add(Internal("cherry", 3), "c").ok());
  ASSERT_TRUE(builder.Finish().ok());

  EXPECT_EQ(ExtractUserKey(builder.SmallestKey()), "apple");
  EXPECT_EQ(ExtractUserKey(builder.LargestKey()), "cherry");
  EXPECT_EQ(builder.EntryCount(), 3U);
}

// A flush interrupted before the footer landed leaves exactly this: a file
// whose tail is missing. It must be rejected, not half-read.
TEST_F(SSTableTest, TruncatedFileIsRejected) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 200; ++i) {
    entries.emplace_back(Internal(testing::NumberedKey(i), static_cast<uint64_t>(i) + 1), "v");
  }
  Build(entries);

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &contents).ok());

  // Every truncation, from "almost complete" down to empty, must be refused.
  for (const size_t keep :
       {contents.size() - 1, contents.size() - 20, contents.size() / 2, size_t{40}, size_t{0}}) {
    ASSERT_TRUE(env_.WriteFile(Path(), std::string_view(contents).substr(0, keep)).ok());
    std::unique_ptr<SSTableReader> reader;
    const Status status = SSTableReader::Open(env_, Path(), &reader);
    EXPECT_TRUE(status.IsCorruption()) << "keep=" << keep << ": " << status.ToString();
  }
}

TEST_F(SSTableTest, CorruptFooterIsRejected) {
  Build({{Internal("apple", 1), "red"}});

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &contents).ok());

  // Damage each byte of the footer in turn. The magic number or the checksum
  // must catch every one.
  for (size_t offset = contents.size() - kFooterSize; offset < contents.size() - 4; ++offset) {
    std::string damaged = contents;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0xFF);
    ASSERT_TRUE(env_.WriteFile(Path(), damaged).ok());

    std::unique_ptr<SSTableReader> reader;
    const Status status = SSTableReader::Open(env_, Path(), &reader);
    EXPECT_TRUE(status.IsCorruption()) << "damaged footer byte at offset " << offset;
  }
}

// The hole this closes. Before per-block checksums, the footer proved only that
// the table was written completely: a bit flipped inside a data block was
// handed back to the caller as a value, silently. Now it is reported.
TEST_F(SSTableTest, CorruptionInsideADataBlockIsDetected) {
  Options options;
  options.block_bytes = 256;

  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 400; ++i) {
    entries.emplace_back(Internal(testing::NumberedKey(i), static_cast<uint64_t>(i) + 1),
                         testing::NumberedValue(i));
  }
  Build(entries, options);

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &contents).ok());

  // Damage a byte well inside the data region, nowhere near the footer or the
  // index -- somewhere only a per-block checksum can catch.
  const size_t target = 64;
  ASSERT_LT(target, contents.size() / 2);
  std::string damaged = contents;
  damaged[target] = static_cast<char>(damaged[target] ^ 0xFF);
  ASSERT_TRUE(env_.WriteFile(Path(), damaged).ok());

  // The table still opens: its footer and index are intact. The damage is only
  // discovered when the block is actually read.
  auto reader = OpenOrDie();

  int corruption_reported = 0;
  int values_returned = 0;
  for (int i = 0; i < 400; ++i) {
    std::string value;
    bool found = false;
    const Status status =
        reader->Get(Lookup(testing::NumberedKey(i)), testing::NumberedKey(i), &value, &found);
    if (status.IsCorruption()) {
      ++corruption_reported;
    } else if (found) {
      ++values_returned;
      // Anything still returned must be correct.
      EXPECT_EQ(value, testing::NumberedValue(i)) << i;
    }
  }

  EXPECT_GT(corruption_reported, 0)
      << "damage inside a data block was not reported: it was returned as a value";
  EXPECT_GT(values_returned, 0) << "undamaged blocks should still be readable";
}

TEST_F(SSTableTest, EveryDataBlockByteIsCoveredByAChecksum) {
  Options options;
  options.block_bytes = 128;

  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 40; ++i) {
    entries.emplace_back(Internal(testing::NumberedKey(i), static_cast<uint64_t>(i) + 1), "v");
  }
  Build(entries, options);

  std::string original;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &original).ok());

  // Walk the whole data region a byte at a time. Every single byte, when
  // damaged, must either be reported or leave every returned value correct --
  // what must never happen is a wrong value returned as if it were right.
  auto reader_for_extent = OpenOrDie();
  const size_t data_region = original.size() / 2;  // Conservatively inside the data blocks.

  int detected = 0;
  for (size_t index = 0; index < data_region; ++index) {
    std::string damaged = original;
    damaged[index] = static_cast<char>(damaged[index] ^ 0xFF);
    ASSERT_TRUE(env_.WriteFile(Path(), damaged).ok());

    std::unique_ptr<SSTableReader> reader;
    if (!SSTableReader::Open(env_, Path(), &reader).ok()) {
      ++detected;  // Caught at open: also fine.
      continue;
    }
    bool any_corruption = false;
    for (int i = 0; i < 40; ++i) {
      std::string value;
      bool found = false;
      const Status status =
          reader->Get(Lookup(testing::NumberedKey(i)), testing::NumberedKey(i), &value, &found);
      if (status.IsCorruption()) {
        any_corruption = true;
      } else if (found) {
        ASSERT_EQ(value, "v") << "byte " << index << " damaged, key " << i
                              << " returned a wrong value as though it were correct";
      }
    }
    if (any_corruption) ++detected;
  }
  EXPECT_GT(detected, 0) << "no damage anywhere in the data region was detected";
}

// The option exists so a caller can trade safety for speed knowingly. If it did
// nothing, it would be a lie in the documentation.
TEST_F(SSTableTest, VerifyChecksumsCanBeTurnedOff) {
  Build({{Internal("apple", 1), "red"}});

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &contents).ok());
  std::string damaged = contents;
  damaged[20] = static_cast<char>(damaged[20] ^ 0xFF);
  ASSERT_TRUE(env_.WriteFile(Path(), damaged).ok());

  auto reader = OpenOrDie();
  std::string value;
  bool found = false;

  // Verification on: the damage is reported.
  EXPECT_TRUE(reader->Get(Lookup("apple"), "apple", &value, &found, /*verify_checksums=*/true)
                  .IsCorruption());

  // Verification off: no checksum is computed, so nothing is reported. The
  // value may be anything; the point is only that the option is honored.
  const Status unverified =
      reader->Get(Lookup("apple"), "apple", &value, &found, /*verify_checksums=*/false);
  EXPECT_FALSE(unverified.IsCorruption())
      << "verify_checksums=false must skip verification, not merely be ignored";
}

TEST_F(SSTableTest, AFileThatIsNotAnSSTableIsRejected) {
  ASSERT_TRUE(env_.WriteFile(Path(), std::string(200, 'x')).ok());
  std::unique_ptr<SSTableReader> reader;
  const Status status = SSTableReader::Open(env_, Path(), &reader);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
  EXPECT_NE(status.ToString().find("magic"), std::string::npos)
      << "the error should say it is not an SSTable: " << status.ToString();
}

TEST_F(SSTableTest, FilterStopsLookupsForAbsentKeys) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 1000; ++i) {
    entries.emplace_back(Internal(testing::NumberedKey(i), static_cast<uint64_t>(i) + 1), "v");
  }
  Build(entries);
  auto reader = OpenOrDie();

  // The filter should reject the overwhelming majority of absent keys outright.
  int rejected = 0;
  constexpr int kProbes = 2000;
  for (int i = 0; i < kProbes; ++i) {
    if (!reader->filter().MayContain("absent-" + std::to_string(i))) ++rejected;
  }
  EXPECT_GT(rejected, kProbes * 9 / 10) << "the filter is not doing its job";

  // And every present key must still be reported as possibly present.
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(reader->filter().MayContain(testing::NumberedKey(i))) << i;
  }
}

TEST_F(SSTableTest, EmptyTableIsValidAndFindsNothing) {
  Build({});
  auto reader = OpenOrDie();
  EXPECT_EQ(reader->BlockCount(), 0U);

  std::string value;
  bool found = false;
  ASSERT_TRUE(reader->Get(Lookup("anything"), "anything", &value, &found).ok());
  EXPECT_FALSE(found);
}

}  // namespace
}  // namespace stratakv
