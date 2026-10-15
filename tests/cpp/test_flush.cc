// Memtable rotation, background flush, the multi-level read path, and crash
// recovery around a flush. These are the tests that prove P2's acceptance
// criteria.

#include <atomic>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "db_impl.h"
#include "env.h"
#include "filename.h"
#include "subprocess.h"
#include "test_util.h"

namespace stratakv {
namespace {

using testing::NumberedKey;
using testing::NumberedValue;
using testing::RunWithEnv;
using testing::SplitLines;
using testing::TempDir;

class FlushTest : public ::testing::Test {
 protected:
  std::string DbPath() const { return dir_.Child("mydb"); }

  // A memtable small enough that a few hundred keys force several rotations.
  Options SmallMemtableOptions() const {
    Options options;
    options.memtable_bytes = 8 * 1024;
    options.sync_policy = SyncPolicy::kNoSync;  // Speed; durability has its own tests.
    return options;
  }

  std::unique_ptr<DB> OpenOrDie(const Options& options) {
    std::unique_ptr<DB> db;
    const Status status = DB::Open(options, DbPath(), &db);
    EXPECT_TRUE(status.ok()) << status.ToString();
    return db;
  }

  std::vector<std::string> ChildrenWithSuffix(std::string_view suffix) const {
    std::vector<std::string> names;
    std::vector<std::string> children;
    if (!Env::Default().GetChildren(DbPath(), &children).ok()) return names;
    for (const std::string& name : children) {
      if (name.size() > 4 && name.substr(name.size() - 4) == suffix) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
};

TEST_F(FlushTest, SmallDatabaseNeverRotates) {
  auto db = OpenOrDie(SmallMemtableOptions());
  ASSERT_TRUE(db->Put(WriteOptions{}, "k", "v").ok());
  ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());

  EXPECT_TRUE(ChildrenWithSuffix(".sst").empty()) << "nothing should have been flushed yet";
}

TEST_F(FlushTest, CrossingTheMemtableLimitProducesAnSSTable) {
  auto db = OpenOrDie(SmallMemtableOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());
  ASSERT_NE(impl, nullptr);

  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok()) << i;
  }
  ASSERT_TRUE(impl->TEST_WaitForFlush().ok());

  EXPECT_GT(ChildrenWithSuffix(".sst").size(), 0U) << "a rotation should have flushed a table";
  EXPECT_GT(impl->TEST_Level0FileCount(), 0U);

  // Everything must still read back, whichever level it now lives in.
  std::string value;
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << "lost key " << i;
    EXPECT_EQ(value, NumberedValue(i)) << i;
  }
}

// The read path has three layers in P2. This drives keys into all of them at
// once and checks each is consulted in the right order.
TEST_F(FlushTest, ReadsSpanMemtableImmutableMemtableAndL0) {
  auto db = OpenOrDie(SmallMemtableOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_WaitForFlush().ok());
  ASSERT_GT(impl->TEST_Level0FileCount(), 1U) << "several tables are needed for this to be a test";

  // Overwrite a key that is certainly in an old table. The newer value lives in
  // the memtable and must win.
  ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(0), "overwritten-in-memtable").ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(0), &value).ok());
  EXPECT_EQ(value, "overwritten-in-memtable") << "the memtable must shadow every table";

  // Delete a key that lives in an old table: the tombstone is newer, so the
  // read must stop at it rather than finding the old value underneath.
  ASSERT_TRUE(db->Delete(WriteOptions{}, NumberedKey(1)).ok());
  EXPECT_TRUE(db->Get(ReadOptions{}, NumberedKey(1), &value).IsNotFound());

  // An untouched key still comes from its table.
  ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(500), &value).ok());
  EXPECT_EQ(value, NumberedValue(500));
}

TEST_F(FlushTest, NewerTablesShadowOlderOnes) {
  auto db = OpenOrDie(SmallMemtableOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  // Write the same key repeatedly, with enough other traffic between writes to
  // force each version into a different table.
  for (int round = 0; round < 6; ++round) {
    ASSERT_TRUE(db->Put(WriteOptions{}, "tracked", "round-" + std::to_string(round)).ok());
    for (int i = 0; i < 400; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(round * 1000 + i), "filler").ok());
    }
  }
  ASSERT_TRUE(impl->TEST_WaitForFlush().ok());
  ASSERT_GT(impl->TEST_Level0FileCount(), 1U);

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "tracked", &value).ok());
  EXPECT_EQ(value, "round-5") << "the newest table must be searched first";
}

// The invariant that makes a flush safe: a log may only be deleted once its
// table is durable.
TEST_F(FlushTest, LogsAreDeletedOnlyAfterTheirTableIsWritten) {
  auto db = OpenOrDie(SmallMemtableOptions());
  auto* impl = dynamic_cast<DBImpl*>(db.get());

  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  ASSERT_TRUE(impl->TEST_WaitForFlush().ok());

  const auto tables = ChildrenWithSuffix(".sst");
  const auto logs = ChildrenWithSuffix(".log");
  EXPECT_GT(tables.size(), 0U);
  // Old logs are removed once flushed, so only the active one should remain.
  EXPECT_EQ(logs.size(), 1U) << "flushed logs should have been deleted";
}

TEST_F(FlushTest, EverythingSurvivesCloseAndReopenAfterManyFlushes) {
  const Options options = SmallMemtableOptions();
  constexpr int kKeys = 5000;
  {
    auto db = OpenOrDie(options);
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  auto db = OpenOrDie(options);
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << "lost key " << i;
    EXPECT_EQ(value, NumberedValue(i)) << i;
  }
}

TEST_F(FlushTest, SequenceNumbersContinueAcrossAFullFlushAndReopen) {
  // If sequence numbering restarted at zero after every log was flushed and
  // deleted, a new write would be ordered *before* the data already on disk and
  // would be invisible. The watermark record in each new log prevents that.
  const Options options = SmallMemtableOptions();
  {
    auto db = OpenOrDie(options);
    for (int i = 0; i < 2000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), "first-generation").ok());
    }
    ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  {
    auto db = OpenOrDie(options);
    // Overwrite a key that is certainly in a table on disk.
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(0), "second-generation").ok());
    std::string value;
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(0), &value).ok());
    EXPECT_EQ(value, "second-generation") << "the new write must be newer than the table's";
    ASSERT_TRUE(db->Close().ok());
  }

  // And it must still win after another reopen.
  auto db = OpenOrDie(options);
  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(0), &value).ok());
  EXPECT_EQ(value, "second-generation");
}

TEST_F(FlushTest, OverwritesAndDeletesSurviveFlushesAndReopen) {
  const Options options = SmallMemtableOptions();
  constexpr int kKeys = 3000;
  std::map<std::string, std::string> model;

  {
    auto db = OpenOrDie(options);
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
      model[NumberedKey(i)] = NumberedValue(i);
    }
    for (int i = 0; i < kKeys; i += 3) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), "overwritten").ok());
      model[NumberedKey(i)] = "overwritten";
    }
    for (int i = 0; i < kKeys; i += 5) {
      ASSERT_TRUE(db->Delete(WriteOptions{}, NumberedKey(i)).ok());
      model.erase(NumberedKey(i));
    }
    ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  auto db = OpenOrDie(options);
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    const std::string key = NumberedKey(i);
    const Status status = db->Get(ReadOptions{}, key, &value);
    const auto expected = model.find(key);
    if (expected == model.end()) {
      EXPECT_TRUE(status.IsNotFound()) << key << " should be deleted";
    } else {
      ASSERT_TRUE(status.ok()) << key;
      EXPECT_EQ(value, expected->second) << key;
    }
  }
}

// A table whose footer never landed is what a crash during a flush leaves. It
// must be discarded on open, and its data recovered from the log that was
// deliberately not deleted yet.
TEST_F(FlushTest, APartiallyWrittenTableIsDiscardedOnOpen) {
  const Options options = SmallMemtableOptions();
  {
    auto db = OpenOrDie(options);
    for (int i = 0; i < 2000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  // Plant a truncated table with a number beyond the existing ones.
  const std::string planted = TableFileName(DbPath(), 9999);
  ASSERT_TRUE(env_.WriteFile(planted, std::string(300, 'x')).ok());

  auto db = OpenOrDie(options);
  EXPECT_FALSE(env_.FileExists(planted)) << "an invalid table should be removed on open";

  std::string value;
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << i;
  }
}

// P2 acceptance: "write 1,000,000 keys (well past one memtable), reopen, read
// all back correctly."
//
// Deliberately uses the DEFAULT 4 MB memtable so this really does span many
// rotations rather than an artificially small one. Batched and unsynced for
// speed: durability has its own tests, and one fsync per key would make this
// run for hours on a drive that honours F_FULLFSYNC.
TEST_F(FlushTest, OneMillionKeysSurviveReopen) {
  constexpr int kKeys = 1000000;
  constexpr int kBatchSize = 1000;

  Options options;  // Default memtable_bytes (4 MB).
  options.sync_policy = SyncPolicy::kNoSync;

  size_t level0_files = 0;
  {
    auto db = OpenOrDie(options);
    for (int base = 0; base < kKeys; base += kBatchSize) {
      WriteBatch batch;
      for (int i = base; i < base + kBatchSize; ++i) {
        batch.Put(NumberedKey(i), NumberedValue(i));
      }
      ASSERT_TRUE(db->Write(WriteOptions{}, &batch).ok()) << "batch at " << base;
    }
    auto* impl = dynamic_cast<DBImpl*>(db.get());
    ASSERT_TRUE(impl->TEST_WaitForFlush().ok());
    level0_files = impl->TEST_Level0FileCount();
    ASSERT_TRUE(db->Close().ok());
  }

  EXPECT_GT(level0_files, 1U) << "1,000,000 keys should have produced several tables";

  // Reopen and demand every single key back with the right value.
  auto db = OpenOrDie(options);
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << "lost key " << i;
    ASSERT_EQ(value, NumberedValue(i)) << "wrong value for key " << i;
  }

  // Absent keys must be reported absent, not resolved to a neighbour.
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(db->Get(ReadOptions{}, "absent-" + std::to_string(i), &value).IsNotFound());
  }
}

// The interaction P2 introduced and nothing else exercises: readers searching
// while the background thread rotates memtables, writes tables, and swaps the
// L0 list underneath them.
//
// Readers hold no lock during their search; they rely on the copied shared_ptrs
// keeping the memtables and open files alive. If that were wrong, this is where
// ThreadSanitizer would say so -- or where a key would vanish mid-flush.
TEST_F(FlushTest, ReadersSeeEveryAcknowledgedKeyWhileFlushesRun) {
  constexpr int kKeys = 4000;
  constexpr int kReaders = 4;

  auto db = OpenOrDie(SmallMemtableOptions());

  // The writer publishes how far it has got; a reader only asserts about keys
  // at or below that, since anything beyond has not been acknowledged yet.
  std::atomic<int> highest_written{-1};
  std::atomic<bool> writing_done{false};
  std::atomic<long> reads_completed{0};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 random(static_cast<uint32_t>(r) + 1);
      std::string value;
      while (!writing_done.load(std::memory_order_acquire)) {
        const int limit = highest_written.load(std::memory_order_acquire);
        if (limit < 0) continue;
        const int index = static_cast<int>(random() % static_cast<uint32_t>(limit + 1));

        // This key was acknowledged before the read began, so no amount of
        // rotation, flushing, or table installation may make it disappear.
        const Status status = db->Get(ReadOptions{}, NumberedKey(index), &value);
        ASSERT_TRUE(status.ok()) << "key " << index
                                 << " vanished during a flush: " << status.ToString();
        ASSERT_EQ(value, NumberedValue(index)) << "key " << index;
        reads_completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok()) << i;
    highest_written.store(i, std::memory_order_release);
  }
  writing_done.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();

  EXPECT_GT(reads_completed.load(std::memory_order_relaxed), 0)
      << "readers never ran, so this proved nothing";

  auto* impl = dynamic_cast<DBImpl*>(db.get());
  ASSERT_TRUE(impl->TEST_WaitForFlush().ok());
  EXPECT_GT(impl->TEST_Level0FileCount(), 0U) << "no flush happened, so this proved nothing";
}

// After a crash that leaves several logs behind, recovery replays all of them
// into one memtable. Every one of those logs must then be deleted when that
// memtable is eventually flushed -- otherwise logs accumulate forever, and the
// database grows without bound even though its contents do not.
TEST_F(FlushTest, LogsLeftByACrashAreCleanedUpByTheNextFlush) {
  const Options options = SmallMemtableOptions();

  // Crash after the table is installed but before its logs are removed, which
  // is precisely the window that leaves more than one log on disk.
  const auto result = RunWithEnv(STRATAKV_CRASH_HELPER, {DbPath(), "4000", "8192"},
                                 "STRATAKV_CRASH_AT", "flush_before_wal_delete");
  ASSERT_TRUE(result.exited_normally);
  ASSERT_EQ(result.exit_code, 1) << "the crash point did not fire";

  const size_t logs_after_crash = ChildrenWithSuffix(".log").size();
  ASSERT_GT(logs_after_crash, 1U) << "the crash should have left more than one log";

  const std::vector<std::string> acknowledged = SplitLines(result.stdout_text);
  ASSERT_GT(acknowledged.size(), 0U);

  {
    auto db = OpenOrDie(options);
    std::string value;
    // Recovery must have replayed every surviving log, not just the newest.
    for (const std::string& key : acknowledged) {
      ASSERT_TRUE(db->Get(ReadOptions{}, key, &value).ok()) << "lost " << key;
    }

    // Write enough to force a rotation and flush of the recovered memtable.
    for (int i = 0; i < 4000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, "post-recovery-" + std::to_string(i), "v").ok());
    }
    ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  EXPECT_LT(ChildrenWithSuffix(".log").size(), logs_after_crash)
      << "the logs the crash left behind were never cleaned up";

  // And nothing was lost in the process.
  auto db = OpenOrDie(options);
  std::string value;
  for (const std::string& key : acknowledged) {
    ASSERT_TRUE(db->Get(ReadOptions{}, key, &value).ok()) << "lost " << key << " after cleanup";
  }
}

// Recovery lists the directory to find its files, so anything else in there
// must be left alone rather than mistaken for a table or a log.
TEST_F(FlushTest, StrayFilesInTheDirectoryAreIgnored) {
  const Options options = SmallMemtableOptions();
  {
    auto db = OpenOrDie(options);
    for (int i = 0; i < 1000; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(dynamic_cast<DBImpl*>(db.get())->TEST_WaitForFlush().ok());
    ASSERT_TRUE(db->Close().ok());
  }

  // Names that look close to ours but are not, plus something unrelated.
  for (const char* name : {"notes.txt", "000001.sstx", "000001.lo", "backup", ".DS_Store"}) {
    ASSERT_TRUE(env_.WriteFile(dir_.Child("mydb/" + std::string(name)), "junk").ok()) << name;
  }

  auto db = OpenOrDie(options);
  std::string value;
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << i;
  }
  // The stray files must still be there: recovery has no business deleting
  // files it does not recognise.
  EXPECT_TRUE(env_.FileExists(dir_.Child("mydb/notes.txt")));
}

// --- P2 acceptance: crash points around the flush ---

// Crash points that fire during a flush. By then thousands of writes have been
// acknowledged, so the interesting question is whether any of them are lost.
class FlushCrashTest : public FlushTest, public ::testing::WithParamInterface<const char*> {};

TEST_P(FlushCrashTest, EveryAcknowledgedWriteSurvivesACrashDuringFlush) {
  const std::string crash_point = GetParam();

  // The child writes keys with a small memtable so a flush is certain, printing
  // each key only after its Put returned OK.
  const auto result = RunWithEnv(STRATAKV_CRASH_HELPER, {DbPath(), "4000", "8192"},
                                 "STRATAKV_CRASH_AT", crash_point);

  // If the crash point never fired, this test proves nothing, so insist on it.
  ASSERT_TRUE(result.exited_normally) << "child was killed by signal " << result.signal;
  ASSERT_EQ(result.exit_code, 1) << "the crash point did not fire; exit code " << result.exit_code;

  const std::vector<std::string> acknowledged = SplitLines(result.stdout_text);
  ASSERT_GT(acknowledged.size(), 0U) << "the child acknowledged nothing at all";

  // Reopen and demand every acknowledged write back.
  const Options options = SmallMemtableOptions();
  auto db = OpenOrDie(options);
  std::string value;
  for (const std::string& key : acknowledged) {
    const Status status = db->Get(ReadOptions{}, key, &value);
    ASSERT_TRUE(status.ok()) << "lost acknowledged write " << key << ": " << status.ToString();
    // The value must be the right one, not merely present: a duplicated or
    // misordered replay would show up here.
    const std::string expected = "value-" + key.substr(4);
    EXPECT_EQ(value, expected) << key;
  }

  // The database must also be usable afterwards.
  ASSERT_TRUE(db->Put(WriteOptions{}, "after-recovery", "ok").ok());
  ASSERT_TRUE(db->Get(ReadOptions{}, "after-recovery", &value).ok());
  EXPECT_EQ(value, "ok");
}

INSTANTIATE_TEST_SUITE_P(AcceptanceCrashPoints, FlushCrashTest,
                         ::testing::Values("flush_after_sst_write", "flush_before_wal_delete"));

// Crash points inside the write path itself fire on the very FIRST write, so
// nothing is ever acknowledged. That makes "no acknowledged write was lost"
// trivially true, and the real question a different one: does the database
// still open and work afterwards?
//
// write_after_wal_sync is the more interesting of the two. The record reached
// the log and was synced, but Put never returned -- so recovery may legitimately
// replay a write nobody was promised. Both outcomes are correct, which is
// exactly why the fuzzer only ever checks acknowledged writes.
class WritePathCrashTest : public FlushTest, public ::testing::WithParamInterface<const char*> {};

TEST_P(WritePathCrashTest, DatabaseStaysUsableAfterACrashInTheWritePath) {
  const std::string crash_point = GetParam();

  const auto result = RunWithEnv(STRATAKV_CRASH_HELPER, {DbPath(), "4000", "8192"},
                                 "STRATAKV_CRASH_AT", crash_point);

  ASSERT_TRUE(result.exited_normally) << "child was killed by signal " << result.signal;
  ASSERT_EQ(result.exit_code, 1) << "the crash point did not fire";

  const std::vector<std::string> acknowledged = SplitLines(result.stdout_text);
  EXPECT_TRUE(acknowledged.empty())
      << "these crash points fire on the first write, so nothing should be acknowledged";

  // Whatever survived must be coherent, and the database must be writable.
  const Options options = SmallMemtableOptions();
  auto db = OpenOrDie(options);
  std::string value;

  const Status first = db->Get(ReadOptions{}, "key-000000", &value);
  if (first.ok()) {
    // Allowed: the record was synced before the process died, it just was not
    // acknowledged. If it is here, it must be correct.
    EXPECT_EQ(value, "value-000000");
  } else {
    EXPECT_TRUE(first.IsNotFound()) << first.ToString();
  }

  ASSERT_TRUE(db->Put(WriteOptions{}, "after-recovery", "ok").ok());
  ASSERT_TRUE(db->Get(ReadOptions{}, "after-recovery", &value).ok());
  EXPECT_EQ(value, "ok");
}

INSTANTIATE_TEST_SUITE_P(WritePathCrashPoints, WritePathCrashTest,
                         ::testing::Values("write_before_wal", "write_after_wal_sync"));

// A crash with no crash point armed should simply complete, which confirms the
// helper and the harness are not themselves the reason a test passes.
TEST_F(FlushTest, TheCrashHelperSucceedsWhenNoCrashPointIsArmed) {
  const auto result =
      RunWithEnv(STRATAKV_CRASH_HELPER, {DbPath(), "500", "8192"}, "STRATAKV_CRASH_AT", "");
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0) << "the helper should finish cleanly with nothing armed";
  EXPECT_EQ(SplitLines(result.stdout_text).size(), 500U);
}

}  // namespace
}  // namespace stratakv
