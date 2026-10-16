#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"

#include "compaction.h"
#include "db_impl.h"
#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "sstable_builder.h"
#include "sstable_reader.h"
#include "subprocess.h"
#include "test_util.h"
#include "version.h"

// The sanitizers run the whole suite 3x to 40x slower. The multi-million-key
// acceptance test is about volume, not about races or memory errors, and the
// concurrency tests that *are* about those run under both sanitizers at a size
// where they finish. Running it instrumented would add half an hour to every
// sweep for no additional signal.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define STRATAKV_UNDER_SANITIZER 1
#endif
#endif
#ifndef STRATAKV_UNDER_SANITIZER
#define STRATAKV_UNDER_SANITIZER 0
#endif

namespace stratakv {
namespace {

using testing::NumberedKey;
using testing::NumberedValue;
using testing::RunWithEnv;
using testing::SplitLines;
using testing::TempDir;

std::string Internal(std::string_view user_key, uint64_t sequence,
                     ValueType type = ValueType::kTypeValue) {
  std::string out;
  AppendInternalKey(&out, user_key, sequence, type);
  return out;
}

// --- Unit-level compaction: exact control over inputs and the snapshot ---

class CompactionUnitTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(env_.CreateDirIfMissing(dir_.path()).ok()); }

  // Writes a table from sorted entries and returns it as a level file.
  LevelFile MakeTable(uint64_t number,
                      const std::vector<std::pair<std::string, std::string>>& entries) {
    const std::string path = TableFileName(dir_.path(), number);
    std::unique_ptr<WritableFile> file;
    EXPECT_TRUE(env_.NewWritableFile(path, &file).ok());
    SSTableBuilder builder(std::move(file), options_);
    for (const auto& [key, value] : entries) {
      EXPECT_TRUE(builder.Add(key, value).ok()) << key;
    }
    EXPECT_TRUE(builder.Finish().ok());

    LevelFile level_file;
    level_file.meta.number = number;
    level_file.meta.file_size = builder.FileSize();
    level_file.meta.smallest = builder.SmallestKey();
    level_file.meta.largest = builder.LargestKey();

    std::unique_ptr<SSTableReader> reader;
    EXPECT_TRUE(SSTableReader::Open(env_, path, &reader).ok());
    level_file.reader = std::move(reader);
    return level_file;
  }

  // Runs a compaction and returns every entry in the outputs, as
  // "user_key@seq[:DEL]".
  std::vector<std::string> Compact(Version& version, uint64_t smallest_snapshot,
                                   VersionEdit* edit = nullptr) {
    VersionEdit local_edit;
    VersionEdit* target = edit != nullptr ? edit : &local_edit;
    CompactionStats stats;
    uint64_t next = 1000;
    const std::function<uint64_t()> allocate = [&next]() { return next++; };

    const Status status = CompactLevel0(env_, dir_.path(), options_, version, smallest_snapshot,
                                        allocate, target, &stats);
    EXPECT_TRUE(status.ok()) << status.ToString();

    std::vector<std::string> entries;
    for (const auto& [level, meta] : target->added_files()) {
      EXPECT_EQ(level, 1) << "compaction output belongs at L1";
      std::unique_ptr<SSTableReader> reader;
      EXPECT_TRUE(SSTableReader::Open(env_, TableFileName(dir_.path(), meta.number), &reader).ok());
      auto iterator = reader->NewIterator();
      for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
        std::string entry = std::string(ExtractUserKey(iterator->key())) + "@" +
                            std::to_string(ExtractSequence(iterator->key()));
        if (ExtractValueType(iterator->key()) == ValueType::kTypeDeletion) entry += ":DEL";
        entries.push_back(entry);
      }
    }
    return entries;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
  Options options_;
};

TEST_F(CompactionUnitTest, NothingToDoWithNoLevel0Files) {
  Version version;
  VersionEdit edit;
  CompactionStats stats;
  uint64_t next = 1;
  const std::function<uint64_t()> allocate = [&next]() { return next++; };
  ASSERT_TRUE(
      CompactLevel0(env_, dir_.path(), options_, version, 100, allocate, &edit, &stats).ok());
  EXPECT_TRUE(edit.added_files().empty());
  EXPECT_TRUE(edit.removed_files().empty());
}

TEST_F(CompactionUnitTest, MergesTwoLevel0FilesIntoOneLevel1File) {
  Version version;
  version.mutable_files(0).push_back(
      MakeTable(2, {{Internal("b", 2), "b2"}, {Internal("d", 2), "d2"}}));
  version.mutable_files(0).push_back(
      MakeTable(1, {{Internal("a", 1), "a1"}, {Internal("c", 1), "c1"}}));

  VersionEdit edit;
  const auto entries = Compact(version, 100, &edit);
  EXPECT_EQ(entries, (std::vector<std::string>{"a@1", "b@2", "c@1", "d@2"}));

  // Both inputs removed from L0, one output added at L1.
  EXPECT_EQ(edit.removed_files().size(), 2U);
  EXPECT_EQ(edit.added_files().size(), 1U);
}

// Space is reclaimed here: an overwritten version stops existing.
TEST_F(CompactionUnitTest, DropsShadowedVersionsWhenNoReadNeedsThem) {
  Version version;
  version.mutable_files(0).push_back(MakeTable(2, {{Internal("k", 20), "new"}}));
  version.mutable_files(0).push_back(MakeTable(1, {{Internal("k", 10), "old"}}));

  // smallest_snapshot above both: nothing can see the old version again.
  const auto entries = Compact(version, 100);
  EXPECT_EQ(entries, (std::vector<std::string>{"k@20"}))
      << "the overwritten version should be gone";
}

// The reason compaction has to know about in-flight reads at all. A read that
// captured snapshot 15 still needs version 10, even though version 20 shadows
// it -- otherwise that read would seek past 20, find nothing, and report an
// existing key as absent.
TEST_F(CompactionUnitTest, KeepsAShadowedVersionThatALiveSnapshotStillNeeds) {
  Version version;
  version.mutable_files(0).push_back(MakeTable(2, {{Internal("k", 20), "new"}}));
  version.mutable_files(0).push_back(MakeTable(1, {{Internal("k", 10), "old"}}));

  const auto entries = Compact(version, 15);
  EXPECT_EQ(entries, (std::vector<std::string>{"k@20", "k@10"}))
      << "a version visible at a live snapshot must survive compaction";
}

TEST_F(CompactionUnitTest, DropsTombstonesAtTheBottomLevel) {
  Version version;
  version.mutable_files(0).push_back(
      MakeTable(2, {{Internal("gone", 20, ValueType::kTypeDeletion), ""}}));
  version.mutable_files(0).push_back(MakeTable(1, {{Internal("gone", 10), "value"}}));

  const auto entries = Compact(version, 100);
  EXPECT_TRUE(entries.empty())
      << "L1 is the bottom level, so both the tombstone and what it hid can go";
}

TEST_F(CompactionUnitTest, KeepsATombstoneALiveSnapshotStillNeeds) {
  Version version;
  version.mutable_files(0).push_back(
      MakeTable(2, {{Internal("gone", 20, ValueType::kTypeDeletion), ""}}));
  version.mutable_files(0).push_back(MakeTable(1, {{Internal("gone", 10), "value"}}));

  // A read at snapshot 15 must still see the value, so neither entry may go.
  const auto entries = Compact(version, 15);
  EXPECT_EQ(entries, (std::vector<std::string>{"gone@20:DEL", "gone@10"}));
}

TEST_F(CompactionUnitTest, OverlappingLevel1FilesJoinTheCompaction) {
  Version version;
  version.mutable_files(0).push_back(MakeTable(2, {{Internal("b", 20), "b-new"}}));
  // Overlaps the L0 range, so it must take part.
  version.mutable_files(1).push_back(
      MakeTable(1, {{Internal("a", 5), "a-old"}, {Internal("b", 5), "b-old"}}));

  VersionEdit edit;
  const auto entries = Compact(version, 100, &edit);
  EXPECT_EQ(entries, (std::vector<std::string>{"a@5", "b@20"}))
      << "the old version of b must be merged away";

  // The L1 input is removed too, not just the L0 one.
  bool removed_from_level1 = false;
  for (const auto& [level, number] : edit.removed_files()) {
    if (level == 1 && number == 1) removed_from_level1 = true;
  }
  EXPECT_TRUE(removed_from_level1);
}

TEST_F(CompactionUnitTest, NonOverlappingLevel1FilesAreLeftAlone) {
  Version version;
  version.mutable_files(0).push_back(MakeTable(2, {{Internal("m", 20), "m"}}));
  // Far away from the L0 range: no reason to rewrite it.
  version.mutable_files(1).push_back(MakeTable(1, {{Internal("zzz", 5), "z"}}));

  VersionEdit edit;
  Compact(version, 100, &edit);

  for (const auto& [level, number] : edit.removed_files()) {
    EXPECT_FALSE(level == 1 && number == 1)
        << "a file that cannot contain any compacted key must not be rewritten";
  }
}

// L1 files must never overlap, because the read path binary searches them.
// Splitting output in the middle of one key's versions would break that.
TEST_F(CompactionUnitTest, OutputSplitsOnlyAtUserKeyBoundaries) {
  options_.l1_file_bytes = 2048;  // Force several output files.
  options_.block_bytes = 256;

  Version version;
  std::vector<std::pair<std::string, std::string>> entries;
  // Many keys, each with three versions, all needing to be kept.
  for (int i = 0; i < 200; ++i) {
    for (const uint64_t sequence : {uint64_t{30}, uint64_t{20}, uint64_t{10}}) {
      entries.emplace_back(Internal(NumberedKey(i), sequence), std::string(50, 'v'));
    }
  }
  version.mutable_files(0).push_back(MakeTable(1, entries));

  VersionEdit edit;
  // A low snapshot keeps every version, so the splitter has to deal with
  // multi-version keys.
  Compact(version, 5, &edit);
  ASSERT_GT(edit.added_files().size(), 1U) << "the output should have been split";

  // No two output files may claim the same user key.
  std::vector<std::pair<std::string, std::string>> ranges;
  for (const auto& [level, meta] : edit.added_files()) {
    ranges.emplace_back(std::string(ExtractUserKey(meta.smallest)),
                        std::string(ExtractUserKey(meta.largest)));
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i) {
    EXPECT_LT(ranges[i - 1].second, ranges[i].first)
        << "output files " << (i - 1) << " and " << i << " overlap on user keys";
  }
}

// --- Database-level compaction ---

class CompactionTest : public ::testing::Test {
 protected:
  std::string DbPath() const { return dir_.Child("mydb"); }

  Options SmallOptions() const {
    Options options;
    options.memtable_bytes = 8 * 1024;
    options.l1_file_bytes = 64 * 1024;
    options.sync_policy = SyncPolicy::kNoSync;
    return options;
  }

  std::unique_ptr<DB> OpenOrDie(const Options& options) {
    std::unique_ptr<DB> db;
    const Status status = DB::Open(options, DbPath(), &db);
    EXPECT_TRUE(status.ok()) << status.ToString();
    return db;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
};

TEST_F(CompactionTest, Level0IsDrainedIntoLevel1) {
  auto db = OpenOrDie(SmallOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());
  ASSERT_NE(impl, nullptr);

  for (int i = 0; i < 4000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_WaitForBackgroundWork().ok());

  EXPECT_GT(impl->TEST_FileCount(1), 0U) << "compaction should have produced L1 files";
  EXPECT_LT(impl->TEST_FileCount(0), 4U) << "L0 should have been drained below the trigger";

  std::string value;
  for (int i = 0; i < 4000; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << "lost key " << i;
    EXPECT_EQ(value, NumberedValue(i)) << i;
  }
}

TEST_F(CompactionTest, Level1FilesNeverOverlap) {
  auto db = OpenOrDie(SmallOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  std::mt19937 random(99);
  for (int i = 0; i < 8000; ++i) {
    const int key = static_cast<int>(random() % 4000);
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(key), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_CompactNow().ok());
  ASSERT_GT(impl->TEST_FileCount(1), 1U) << "several L1 files are needed for this to be a test";

  // Read every key back: an overlap would show up as a missing or stale value,
  // because the read path binary searches L1 assuming no overlap.
  std::string value;
  for (int i = 0; i < 4000; ++i) {
    const Status status = db->Get(ReadOptions{}, NumberedKey(i), &value);
    EXPECT_TRUE(status.ok() || status.IsNotFound()) << status.ToString();
  }
}

TEST_F(CompactionTest, CompactionReclaimsSpaceFromOverwrites) {
  // Automatic compaction is held off so the before/after measurement means
  // something: otherwise the background thread compacts while the data is
  // still being written, and "before" is already compacted.
  Options options = SmallOptions();
  options.l0_compaction_trigger = 100000;
  auto db = OpenOrDie(options);
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  // Write the same 200 keys over and over: almost everything is garbage.
  for (int round = 0; round < 40; ++round) {
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), std::string(100, 'a' + round % 26)).ok());
    }
  }
  ASSERT_TRUE(impl->TEST_WaitForBackgroundWork().ok());
  const uint64_t before = impl->TEST_TotalFileBytes();
  ASSERT_GT(impl->TEST_FileCount(0), 1U) << "several L0 files are needed for this to be a test";

  ASSERT_TRUE(impl->TEST_CompactNow().ok());
  const uint64_t after = impl->TEST_TotalFileBytes();

  EXPECT_LT(after, before) << "compacting 40 generations of 200 keys must reclaim space: " << before
                           << " -> " << after;

  std::string value;
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << i;
  }
}

TEST_F(CompactionTest, DeletedKeysStayDeletedAcrossCompactionAndReopen) {
  const Options options = SmallOptions();
  {
    auto db = OpenOrDie(options);
    auto* impl = dynamic_cast<DBImpl*>(db.get());
    for (int i = 0; i < 3000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    for (int i = 0; i < 3000; i += 2) {
      ASSERT_TRUE(db->Delete(WriteOptions{}, NumberedKey(i)).ok());
    }
    ASSERT_TRUE(impl->TEST_CompactNow().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  auto db = OpenOrDie(options);
  std::string value;
  for (int i = 0; i < 3000; ++i) {
    const Status status = db->Get(ReadOptions{}, NumberedKey(i), &value);
    if (i % 2 == 0) {
      EXPECT_TRUE(status.IsNotFound()) << "key " << i << " came back from the dead";
    } else {
      ASSERT_TRUE(status.ok()) << i;
      EXPECT_EQ(value, NumberedValue(i));
    }
  }
}

TEST_F(CompactionTest, ObsoleteFilesAreDeletedAfterCompaction) {
  auto db = OpenOrDie(SmallOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  for (int i = 0; i < 4000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_CompactNow().ok());

  // Every .sst on disk must be one the current version references, or disk
  // usage would grow forever.
  std::vector<std::string> children;
  ASSERT_TRUE(env_.GetChildren(DbPath(), &children).ok());
  size_t tables_on_disk = 0;
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (ParseFileName(name, &number, &type) && type == FileType::kTable) ++tables_on_disk;
  }
  EXPECT_EQ(tables_on_disk, impl->TEST_FileCount(0) + impl->TEST_FileCount(1))
      << "files no version references should have been deleted";
}

TEST_F(CompactionTest, ReadersAreUnaffectedByAConcurrentCompaction) {
  auto db = OpenOrDie(SmallOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  constexpr int kKeys = 3000;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_WaitForBackgroundWork().ok());

  std::atomic<bool> stop{false};
  std::atomic<long> reads{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 random(static_cast<uint32_t>(r) + 7);
      std::string value;
      while (!stop.load(std::memory_order_acquire)) {
        const int index = static_cast<int>(random() % kKeys);
        // These were all acknowledged before any reader started, so compaction
        // installing new versions must never make one unreadable.
        const Status status = db->Get(ReadOptions{}, NumberedKey(index), &value);
        ASSERT_TRUE(status.ok()) << "key " << index
                                 << " lost during compaction: " << status.ToString();
        ASSERT_EQ(value, NumberedValue(index)) << index;
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // EXPECT rather than ASSERT: an ASSERT here returns from the test body
  // immediately, leaving `stop` unset and the reader threads running against
  // locals that are about to be destroyed -- and a joinable std::thread whose
  // destructor runs calls std::terminate. The failure has to be recorded and
  // then fall through to the cleanup below.
  Status compaction_status;
  for (int round = 0; round < 3; ++round) {
    const Status status = impl->TEST_CompactNow();
    if (compaction_status.ok()) compaction_status = status;
  }
  EXPECT_TRUE(compaction_status.ok()) << compaction_status.ToString();

  // Three compactions can finish before the reader threads are even scheduled,
  // in which case `stop` would be set before a single read happened and the
  // guard below would fail for a reason that has nothing to do with the engine.
  // Waiting for real reads makes the test depend on behavior rather than on
  // thread start-up timing. Bounded, so a genuinely stuck reader fails the
  // assertion instead of hanging the suite.
  constexpr long kMinimumReads = 200;
  for (int waited = 0; reads.load(std::memory_order_relaxed) < kMinimumReads && waited < 10000;
       ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();

  EXPECT_GE(reads.load(std::memory_order_relaxed), kMinimumReads)
      << "readers did not get far enough for this to prove anything";
}

// Reads, writes, and compactions all overlapping, with every key always
// present: a NotFound here is unambiguously a bug.
//
// Worth being precise about what this does and does not prove. It was run with
// the read-snapshot registry deliberately disabled, and it still PASSED. The
// window the registry protects is far too narrow to hit by chance: a read
// completes in microseconds, while the compaction that would have to finish
// inside it takes milliseconds. So this test is a useful smoke test for the
// concurrent paths, but it is NOT what justifies the registry.
//
// The deterministic unit tests above -- KeepsAShadowedVersionThatALiveSnapshot
// StillNeeds and KeepsATombstoneALiveSnapshotStillNeeds -- are the real proof,
// because they state the rule directly instead of hoping to race into it. That
// is the general lesson: a correctness rule that only matters in a rare
// interleaving has to be tested by asserting the rule, not by running the
// system and waiting.
TEST_F(CompactionTest, ReadsNeverMissAKeyWhileWritesAndCompactionsOverlap) {
  constexpr int kKeys = 60;  // Small, so versions pile up fast.
  auto db = OpenOrDie(SmallOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  // Every key exists before any reader starts, and none is ever deleted.
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), "initial").ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<long> reads{0};
  std::atomic<long> not_found{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 random(static_cast<uint32_t>(r) + 31);
      std::string value;
      while (!stop.load(std::memory_order_acquire)) {
        const int index = static_cast<int>(random() % kKeys);
        if (db->Get(ReadOptions{}, NumberedKey(index), &value).IsNotFound()) {
          not_found.fetch_add(1, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Writers churn versions while compactions collapse them.
  std::thread writer([&] {
    std::mt19937 random(7);
    for (int i = 0; i < 40000 && !stop.load(std::memory_order_acquire); ++i) {
      const int index = static_cast<int>(random() % kKeys);
      (void)db->Put(WriteOptions{}, NumberedKey(index), "v" + std::to_string(i));
    }
  });

  for (int round = 0; round < 8; ++round) {
    (void)impl->TEST_CompactNow();
  }
  writer.join();
  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();

  EXPECT_GT(reads.load(std::memory_order_relaxed), 1000L) << "too few reads to prove anything";
  EXPECT_EQ(not_found.load(std::memory_order_relaxed), 0L)
      << "a key that always existed read as absent: compaction dropped a version that a "
         "read in flight still needed";
}

// A compaction that crashed before its MANIFEST record landed leaves output
// files nothing references. They must be cleaned up, or every crash during a
// compaction would leak disk space permanently.
TEST_F(CompactionTest, OrphanedCompactionOutputsAreDeletedOnOpen) {
  const auto result = RunWithEnv(STRATAKV_CRASH_HELPER, {DbPath(), "6000", "8192"},
                                 "STRATAKV_CRASH_AT", "compaction_before_manifest");
  ASSERT_TRUE(result.exited_normally);
  ASSERT_EQ(result.exit_code, 1) << "the crash point never fired";

  const auto count_tables = [this]() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_.GetChildren(DbPath(), &children).ok());
    size_t tables = 0;
    for (const std::string& name : children) {
      uint64_t number = 0;
      FileType type{};
      if (ParseFileName(name, &number, &type) && type == FileType::kTable) ++tables;
    }
    return tables;
  };

  const size_t tables_after_crash = count_tables();
  ASSERT_GT(tables_after_crash, 0U);

  auto db = OpenOrDie(SmallOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());
  ASSERT_TRUE(impl->TEST_WaitForBackgroundWork().ok());

  // Whatever remains must be referenced by the current version.
  EXPECT_EQ(count_tables(), impl->TEST_FileCount(0) + impl->TEST_FileCount(1))
      << "orphaned compaction outputs were not cleaned up";
}

// The MANIFEST is authoritative, so a file it lists but which is gone is a real
// inconsistency -- unlike P2, where the directory scan simply would not see it.
// It must be reported rather than opening a database with a hole in it.
TEST_F(CompactionTest, AFileTheManifestListsButIsMissingIsReported) {
  const Options options = SmallOptions();
  {
    auto db = OpenOrDie(options);
    auto* impl = dynamic_cast<DBImpl*>(db.get());
    for (int i = 0; i < 4000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(impl->TEST_CompactNow().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  // Delete a table the MANIFEST references.
  std::vector<std::string> children;
  ASSERT_TRUE(env_.GetChildren(DbPath(), &children).ok());
  std::string victim;
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type{};
    if (ParseFileName(name, &number, &type) && type == FileType::kTable) {
      victim = name;
      break;
    }
  }
  ASSERT_FALSE(victim.empty()) << "no table to remove";
  ASSERT_TRUE(env_.RemoveFile(DbPath() + "/" + victim).ok());

  std::unique_ptr<DB> db;
  const Status status = DB::Open(options, DbPath(), &db);
  EXPECT_FALSE(status.ok()) << "a database missing a referenced table must not open silently";
  EXPECT_EQ(db, nullptr);
}

// The failure branches, finally testable. Both the flush and the compaction
// record a sticky error and stop accepting writes rather than carrying on with
// a memtable that can never be flushed or a level that can never be compacted.
//
// Until P4 added fault injection to Env these three lines were reviewed but
// never executed, which the P2 and P3 debriefs said outright.
class FailureInjectionTest : public CompactionTest {
 protected:
  void TearDown() override { Env::SetFailureInjectionForTesting("", 0); }
};

TEST_F(FailureInjectionTest, AFailedFlushStopsAcceptingWrites) {
  // Automatic compaction is held off so the only thing that can create a table
  // is the flush this test is about -- otherwise a compaction could consume the
  // single armed failure instead and the test would pass for the wrong reason.
  Options options = SmallOptions();
  options.l0_compaction_trigger = 100000;
  auto db = OpenOrDie(options);
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  // Fail the next table the flush tries to create.
  Env::SetFailureInjectionForTesting(".sst", 1);

  // Write until a write is refused. The sticky error reaches a writer at its
  // next rotation, so this is bounded rather than immediate.
  Status write_status;
  for (int i = 0; i < 40000 && write_status.ok(); ++i) {
    write_status = db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i));
  }
  EXPECT_FALSE(write_status.ok())
      << "a failed flush must eventually refuse writes rather than be swallowed";
  EXPECT_FALSE(impl->TEST_WaitForBackgroundWork().ok()) << "the error must be reported";

  // And it must stay refused: a memtable that cannot be flushed will never free
  // its log, so carrying on would accumulate state forever.
  Env::SetFailureInjectionForTesting("", 0);
  EXPECT_FALSE(db->Put(WriteOptions{}, "after-failure", "v").ok())
      << "writes must stay refused after a flush failure";
}

TEST_F(FailureInjectionTest, AFailedCompactionStopsAcceptingWrites) {
  // Automatic compaction is held off for a reason that cost a CI failure to
  // learn: with it on, the background thread may already have drained L0 by the
  // time the failure is armed, leaving the requested compaction with nothing to
  // do -- so it succeeds, and the test fails for a reason unrelated to what it
  // is testing. Holding it off guarantees there is work to fail.
  Options options = SmallOptions();
  options.l0_compaction_trigger = 100000;
  auto db = OpenOrDie(options);
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  for (int i = 0; i < 4000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_WaitForBackgroundWork().ok());
  ASSERT_GT(impl->TEST_FileCount(0), 0U)
      << "a compaction needs L0 files before it can be made to fail";

  // Fail the compaction's output file.
  Env::SetFailureInjectionForTesting(".sst", 1);
  const Status compaction = impl->TEST_CompactNow();
  EXPECT_FALSE(compaction.ok()) << "a failed compaction must be reported";

  Env::SetFailureInjectionForTesting("", 0);
  EXPECT_FALSE(db->Put(WriteOptions{}, "after-failure", "v").ok())
      << "writes must stay refused after a compaction failure";
}

TEST_F(FailureInjectionTest, InjectionOnlyAffectsMatchingPathsAndIsBounded) {
  // The hook itself has to be trustworthy, or the two tests above prove nothing.
  Env::SetFailureInjectionForTesting("no-such-file", 1);
  auto db = OpenOrDie(SmallOptions());
  EXPECT_TRUE(db->Put(WriteOptions{}, "unaffected", "v").ok())
      << "a non-matching path must not be failed";

  Env::SetFailureInjectionForTesting("", 0);
  std::string value;
  EXPECT_TRUE(db->Get(ReadOptions{}, "unaffected", &value).ok());
}

// --- P3 acceptance criteria ---

// "After writing 5,000,000 random keys with 30% overwrites and 10% deletes, the
// database matches a reference std::map", and "disk usage after compaction is
// within 1.5x of live data size."
TEST_F(CompactionTest, MillionsOfOperationsMatchAReferenceMap) {
  if (STRATAKV_UNDER_SANITIZER) {
    GTEST_SKIP() << "volume test; the sanitizers cover concurrency at a smaller size";
  }

  constexpr int kOperations = 5000000;
  constexpr int kKeySpace = 400000;
  constexpr int kBatchSize = 1000;

  Options options;  // Default 4 MB memtable and 8 MB L1 files.
  options.sync_policy = SyncPolicy::kNoSync;

  std::map<std::string, std::string> model;
  auto db = OpenOrDie(options);
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  std::mt19937 random(20261015);
  for (int done = 0; done < kOperations; done += kBatchSize) {
    WriteBatch batch;
    for (int i = 0; i < kBatchSize; ++i) {
      const int key_index = static_cast<int>(random() % kKeySpace);
      const std::string key = NumberedKey(key_index);
      const uint32_t roll = random() % 100;
      if (roll < 10) {
        // 10% deletes.
        batch.Delete(key);
        model.erase(key);
      } else {
        // The rest are writes; because the key space is smaller than the
        // operation count, roughly 30% of them land on a key that already
        // exists and are therefore overwrites.
        const std::string value = NumberedValue(done + i);
        batch.Put(key, value);
        model[key] = value;
      }
    }
    ASSERT_TRUE(db->Write(WriteOptions{}, &batch).ok()) << "batch at " << done;
  }

  ASSERT_TRUE(impl->TEST_WaitForBackgroundWork().ok());
  ASSERT_TRUE(impl->TEST_CompactNow().ok());

  // Every key the model holds must be present with the right value, and every
  // key it does not must be absent.
  std::string value;
  for (const auto& [key, expected] : model) {
    ASSERT_TRUE(db->Get(ReadOptions{}, key, &value).ok()) << "missing " << key;
    ASSERT_EQ(value, expected) << key;
  }
  int checked_absent = 0;
  for (int i = 0; i < kKeySpace && checked_absent < 20000; ++i) {
    const std::string key = NumberedKey(i);
    if (model.count(key) > 0) continue;
    ++checked_absent;
    ASSERT_TRUE(db->Get(ReadOptions{}, key, &value).IsNotFound()) << key << " should be absent";
  }
  EXPECT_GT(checked_absent, 0) << "no deleted keys were checked, so this proved little";

  // Space amplification: disk usage within 1.5x of the live data.
  uint64_t live_bytes = 0;
  for (const auto& [key, stored] : model) live_bytes += key.size() + stored.size();
  const uint64_t disk_bytes = impl->TEST_TotalFileBytes();

  const double ratio = static_cast<double>(disk_bytes) / static_cast<double>(live_bytes);
  std::cout << "space amplification after compaction: " << ratio << "x (" << disk_bytes
            << " bytes on disk, " << live_bytes << " bytes live across " << model.size() << " keys)"
            << std::endl;
  EXPECT_LT(ratio, 1.5) << "disk usage must be within 1.5x of live data after compaction";

  ASSERT_TRUE(db->Close().ok());
}

// "Crash points compaction_after_output and compaction_before_manifest both
// recover correctly."
class CompactionCrashTest : public CompactionTest,
                            public ::testing::WithParamInterface<const char*> {};

TEST_P(CompactionCrashTest, EveryAcknowledgedWriteSurvivesACrashDuringCompaction) {
  const std::string crash_point = GetParam();

  // Enough keys with a small memtable to produce four L0 files and so trigger a
  // compaction before the child finishes.
  const auto result = RunWithEnv(STRATAKV_CRASH_HELPER, {DbPath(), "6000", "8192"},
                                 "STRATAKV_CRASH_AT", crash_point);

  ASSERT_TRUE(result.exited_normally) << "child was killed by signal " << result.signal;
  ASSERT_EQ(result.exit_code, 1) << "the crash point never fired, so this proved nothing";

  const std::vector<std::string> acknowledged = SplitLines(result.stdout_text);
  ASSERT_GT(acknowledged.size(), 0U) << "the child acknowledged nothing";

  auto db = OpenOrDie(SmallOptions());
  std::string value;
  for (const std::string& key : acknowledged) {
    const Status status = db->Get(ReadOptions{}, key, &value);
    ASSERT_TRUE(status.ok()) << "lost acknowledged write " << key << ": " << status.ToString();
    EXPECT_EQ(value, "value-" + key.substr(4)) << key;
  }

  // And the database must still work, including compacting again.
  auto* impl = dynamic_cast<DBImpl*>(db.get());
  ASSERT_TRUE(db->Put(WriteOptions{}, "after-recovery", "ok").ok());
  ASSERT_TRUE(impl->TEST_CompactNow().ok());
  ASSERT_TRUE(db->Get(ReadOptions{}, "after-recovery", &value).ok());
  EXPECT_EQ(value, "ok");
}

INSTANTIATE_TEST_SUITE_P(AcceptanceCompactionCrashPoints, CompactionCrashTest,
                         ::testing::Values("compaction_after_output",
                                           "compaction_before_manifest"));

}  // namespace
}  // namespace stratakv
