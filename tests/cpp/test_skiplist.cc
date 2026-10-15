#include <atomic>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "arena.h"
#include "internal_key.h"
#include "skiplist.h"

namespace stratakv {
namespace {

// The skip list is ordered by InternalKeyComparator, so test keys are built as
// real internal keys.
std::string Key(std::string_view user_key, uint64_t sequence) {
  std::string out;
  AppendInternalKey(&out, user_key, sequence, ValueType::kTypeValue);
  return out;
}

class SkipListTest : public ::testing::Test {
 protected:
  InternalKeyComparator comparator_;
  Arena arena_;
  SkipList list_{comparator_, &arena_};
};

TEST_F(SkipListTest, EmptyListFindsNothing) {
  SkipList::Iterator iterator(&list_);
  iterator.SeekToFirst();
  EXPECT_FALSE(iterator.Valid());

  iterator.Seek(Key("anything", 1));
  EXPECT_FALSE(iterator.Valid());
}

TEST_F(SkipListTest, InsertThenSeekFindsTheEntry) {
  list_.Insert(Key("apple", 1), "red");

  SkipList::Iterator iterator(&list_);
  iterator.Seek(Key("apple", 1));
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(ExtractUserKey(iterator.key()), "apple");
  EXPECT_EQ(iterator.value(), "red");
}

TEST_F(SkipListTest, IterationIsSortedRegardlessOfInsertionOrder) {
  for (const auto* name : {"pear", "apple", "fig", "banana", "date"}) {
    list_.Insert(Key(name, 1), name);
  }

  std::vector<std::string> seen;
  SkipList::Iterator iterator(&list_);
  for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
    seen.emplace_back(ExtractUserKey(iterator.key()));
  }

  const std::vector<std::string> expected = {"apple", "banana", "date", "fig", "pear"};
  EXPECT_EQ(seen, expected);
}

TEST_F(SkipListTest, NewerVersionsOfOneKeySortFirst) {
  list_.Insert(Key("apple", 1), "oldest");
  list_.Insert(Key("apple", 5), "newest");
  list_.Insert(Key("apple", 3), "middle");

  SkipList::Iterator iterator(&list_);
  iterator.Seek(Key("apple", kMaxSequenceNumber));
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(iterator.value(), "newest") << "the highest sequence number must come first";

  iterator.Next();
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(iterator.value(), "middle");
  iterator.Next();
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(iterator.value(), "oldest");
}

TEST_F(SkipListTest, SeekLandsOnTheFirstEntryAtOrAfterTheTarget) {
  list_.Insert(Key("a", 1), "1");
  list_.Insert(Key("c", 1), "3");

  SkipList::Iterator iterator(&list_);
  iterator.Seek(Key("b", 1));
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(ExtractUserKey(iterator.key()), "c") << "b is absent, so seek lands on c";

  iterator.Seek(Key("z", 1));
  EXPECT_FALSE(iterator.Valid()) << "nothing sorts after z";
}

TEST_F(SkipListTest, StoresValuesIndependentlyOfCallerBuffers) {
  // The list copies into the arena, so a caller's buffer going away or being
  // reused must not affect what is stored.
  {
    std::string temporary_key = Key("ephemeral", 1);
    std::string temporary_value = "copied";
    list_.Insert(temporary_key, temporary_value);
    temporary_key.assign(temporary_key.size(), 'X');
    temporary_value.assign(temporary_value.size(), 'Y');
  }

  SkipList::Iterator iterator(&list_);
  iterator.SeekToFirst();
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(ExtractUserKey(iterator.key()), "ephemeral");
  EXPECT_EQ(iterator.value(), "copied");
}

TEST_F(SkipListTest, HandlesEmptyValues) {
  list_.Insert(Key("tombstone-like", 1), "");
  SkipList::Iterator iterator(&list_);
  iterator.SeekToFirst();
  ASSERT_TRUE(iterator.Valid());
  EXPECT_EQ(iterator.value(), "");
}

// Enough entries that several levels of express lanes get built and exercised.
TEST_F(SkipListTest, ManyRandomInsertsStaySortedAndComplete) {
  constexpr int kCount = 20000;
  std::mt19937 random(12345);
  std::set<int> inserted;

  while (static_cast<int>(inserted.size()) < kCount) {
    const int value = static_cast<int>(random() % 1000000);
    if (!inserted.insert(value).second) continue;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%08d", value);
    list_.Insert(Key(buffer, 1), buffer);
  }

  // Walking the list must reproduce the sorted set exactly.
  std::vector<std::string> expected;
  expected.reserve(inserted.size());
  for (const int value : inserted) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%08d", value);
    expected.emplace_back(buffer);
  }

  std::vector<std::string> seen;
  seen.reserve(expected.size());
  SkipList::Iterator iterator(&list_);
  for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
    seen.emplace_back(ExtractUserKey(iterator.key()));
  }

  EXPECT_EQ(seen, expected);

  // And every entry must be findable by seek, not just by walking.
  for (const std::string& key : expected) {
    iterator.Seek(Key(key, 1));
    ASSERT_TRUE(iterator.Valid()) << key;
    EXPECT_EQ(ExtractUserKey(iterator.key()), key);
  }
}

// The reason this structure was chosen over std::map behind a lock, and the
// test that gives ThreadSanitizer something to prove. One writer inserts while
// readers scan with no lock at all; every scan must see a sorted list whose
// values match their keys. A missing acquire/release pairing shows up here as a
// TSan report, or as a reader seeing a node before its contents.
TEST_F(SkipListTest, ReadersScanSafelyWhileOneWriterInserts) {
  constexpr int kEntries = 5000;
  constexpr int kReaders = 4;

  std::atomic<bool> writing_done{false};
  std::atomic<int> scans_completed{0};

  std::thread writer([&] {
    for (int i = 0; i < kEntries; ++i) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%08d", i);
      list_.Insert(Key(buffer, 1), buffer);
    }
    writing_done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      size_t highest_count = 0;
      while (!writing_done.load(std::memory_order_acquire)) {
        std::string previous;
        size_t count = 0;
        SkipList::Iterator iterator(&list_);
        for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
          const std::string user_key(ExtractUserKey(iterator.key()));
          // Torn publication would show up as a value that does not match its
          // own key, or as an out-of-order entry.
          ASSERT_EQ(iterator.value(), user_key);
          if (!previous.empty()) ASSERT_LT(previous, user_key) << "list went out of order";
          previous = user_key;
          ++count;
        }
        // The list only grows, so a scan must never see fewer entries than a
        // previous one by this same reader.
        ASSERT_GE(count, highest_count);
        highest_count = count;
        scans_completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& reader : readers) reader.join();

  EXPECT_GT(scans_completed.load(std::memory_order_relaxed), 0)
      << "readers never ran, so this proved nothing";

  // Everything the writer inserted must be present at the end.
  size_t final_count = 0;
  SkipList::Iterator iterator(&list_);
  for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) ++final_count;
  EXPECT_EQ(final_count, static_cast<size_t>(kEntries));
}

TEST(Arena, AllocatesAlignedAndIndependentRegions) {
  Arena arena;
  for (const size_t alignment : {size_t{1}, size_t{2}, size_t{8}, size_t{16}}) {
    char* block = arena.Allocate(32, alignment);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(block) % alignment, 0U) << "alignment " << alignment;
  }

  // Two allocations must not overlap.
  char* first = arena.Allocate(16, 1);
  char* second = arena.Allocate(16, 1);
  std::fill_n(first, 16, 'a');
  std::fill_n(second, 16, 'b');
  EXPECT_EQ(first[0], 'a');
  EXPECT_EQ(second[0], 'b');
}

TEST(Arena, CopyBytesSurvivesTheSourceAndHandlesEmpty) {
  Arena arena;
  std::string_view copied;
  {
    const std::string source = "temporary contents";
    copied = arena.CopyBytes(source);
  }
  EXPECT_EQ(copied, "temporary contents");
  EXPECT_TRUE(arena.CopyBytes("").empty());
}

TEST(Arena, GrowsPastOneBlockAndReportsUsage) {
  Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0U);
  arena.Allocate(1, 1);
  const size_t after_first = arena.MemoryUsage();
  EXPECT_GT(after_first, 0U);

  // An allocation far larger than the block size must still work.
  char* big = arena.Allocate(100000, 8);
  ASSERT_NE(big, nullptr);
  std::fill_n(big, 100000, 'z');
  EXPECT_GT(arena.MemoryUsage(), after_first);
}

}  // namespace
}  // namespace stratakv
