#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "env.h"
#include "test_util.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

using testing::NumberedKey;
using testing::NumberedValue;
using testing::TempDir;

class DBTest : public ::testing::Test {
 protected:
  std::string DbPath() const { return dir_.Child("mydb"); }
  std::string WalPath() const { return DbPath() + "/000001.log"; }

  Status Open(std::unique_ptr<DB>* db, Options options = Options{}) {
    return DB::Open(options, DbPath(), db);
  }

  std::unique_ptr<DB> OpenOrDie(Options options = Options{}) {
    std::unique_ptr<DB> db;
    const Status status = Open(&db, options);
    EXPECT_TRUE(status.ok()) << status.ToString();
    return db;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
};

TEST_F(DBTest, OpenCreatesTheDirectoryAndItsFiles) {
  auto db = OpenOrDie();
  ASSERT_NE(db, nullptr);
  EXPECT_TRUE(env_.FileExists(DbPath()));
  EXPECT_TRUE(env_.FileExists(DbPath() + "/LOCK"));
}

TEST_F(DBTest, RespectsCreateIfMissingAndErrorIfExists) {
  Options no_create;
  no_create.create_if_missing = false;
  std::unique_ptr<DB> db;
  EXPECT_TRUE(Open(&db, no_create).IsInvalidArgument());

  db = OpenOrDie();
  ASSERT_TRUE(db->Close().ok());
  db.reset();

  Options error_if_exists;
  error_if_exists.error_if_exists = true;
  std::unique_ptr<DB> second;
  EXPECT_TRUE(Open(&second, error_if_exists).IsInvalidArgument());
}

TEST_F(DBTest, PutThenGet) {
  auto db = OpenOrDie();
  ASSERT_TRUE(db->Put(WriteOptions{}, "apple", "red").ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "apple", &value).ok());
  EXPECT_EQ(value, "red");
}

TEST_F(DBTest, GetOnAMissingKeyReturnsNotFound) {
  auto db = OpenOrDie();
  std::string value;
  EXPECT_TRUE(db->Get(ReadOptions{}, "absent", &value).IsNotFound());
}

TEST_F(DBTest, OverwriteReturnsTheNewestValue) {
  auto db = OpenOrDie();
  ASSERT_TRUE(db->Put(WriteOptions{}, "k", "one").ok());
  ASSERT_TRUE(db->Put(WriteOptions{}, "k", "two").ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "k", &value).ok());
  EXPECT_EQ(value, "two");
}

TEST_F(DBTest, DeleteHidesTheKeyAndAReWriteRestoresIt) {
  auto db = OpenOrDie();
  ASSERT_TRUE(db->Put(WriteOptions{}, "k", "v").ok());
  ASSERT_TRUE(db->Delete(WriteOptions{}, "k").ok());

  std::string value;
  EXPECT_TRUE(db->Get(ReadOptions{}, "k", &value).IsNotFound());

  ASSERT_TRUE(db->Put(WriteOptions{}, "k", "again").ok());
  ASSERT_TRUE(db->Get(ReadOptions{}, "k", &value).ok());
  EXPECT_EQ(value, "again");
}

TEST_F(DBTest, DeletingAKeyThatNeverExistedIsNotAnError) {
  auto db = OpenOrDie();
  EXPECT_TRUE(db->Delete(WriteOptions{}, "never-there").ok());
  std::string value;
  EXPECT_TRUE(db->Get(ReadOptions{}, "never-there", &value).IsNotFound());
}

TEST_F(DBTest, HandlesEmptyAndBinaryKeysAndValues) {
  const std::string binary_key("k\0ey\xFF", 5);
  const std::string binary_value("v\0al\xFF", 5);

  auto db = OpenOrDie();
  ASSERT_TRUE(db->Put(WriteOptions{}, "", "empty-key").ok());
  ASSERT_TRUE(db->Put(WriteOptions{}, binary_key, binary_value).ok());
  ASSERT_TRUE(db->Put(WriteOptions{}, "empty-value", "").ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "", &value).ok());
  EXPECT_EQ(value, "empty-key");
  ASSERT_TRUE(db->Get(ReadOptions{}, binary_key, &value).ok());
  EXPECT_EQ(value, binary_value);
  ASSERT_TRUE(db->Get(ReadOptions{}, "empty-value", &value).ok());
  EXPECT_EQ(value, "");
}

TEST_F(DBTest, WriteBatchAppliesEveryOperation) {
  auto db = OpenOrDie();
  ASSERT_TRUE(db->Put(WriteOptions{}, "existing", "old").ok());

  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("existing");
  ASSERT_TRUE(db->Write(WriteOptions{}, &batch).ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "a", &value).ok());
  EXPECT_EQ(value, "1");
  ASSERT_TRUE(db->Get(ReadOptions{}, "b", &value).ok());
  EXPECT_EQ(value, "2");
  EXPECT_TRUE(db->Get(ReadOptions{}, "existing", &value).IsNotFound());
}

TEST_F(DBTest, AnEmptyBatchIsANoOp) {
  auto db = OpenOrDie();
  WriteBatch batch;
  EXPECT_TRUE(db->Write(WriteOptions{}, &batch).ok());
  EXPECT_TRUE(db->Write(WriteOptions{}, nullptr).IsInvalidArgument());
}

TEST_F(DBTest, LaterOperationsInOneBatchWinOverEarlierOnes) {
  auto db = OpenOrDie();
  WriteBatch batch;
  batch.Put("k", "first");
  batch.Put("k", "second");
  ASSERT_TRUE(db->Write(WriteOptions{}, &batch).ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "k", &value).ok());
  EXPECT_EQ(value, "second") << "operations get consecutive sequence numbers, so the last wins";
}

TEST_F(DBTest, TwoProcessesCannotHoldOneDatabase) {
  auto first = OpenOrDie();

  // flock belongs to the open file description, so a second open conflicts even
  // from inside this one process.
  std::unique_ptr<DB> second;
  const Status status = Open(&second);
  EXPECT_TRUE(status.IsIOError()) << status.ToString();
  EXPECT_EQ(second, nullptr);
}

TEST_F(DBTest, ClosingReleasesTheLockSoTheDatabaseCanBeReopened) {
  auto first = OpenOrDie();
  ASSERT_TRUE(first->Close().ok());
  first.reset();

  auto second = OpenOrDie();
  EXPECT_NE(second, nullptr);
}

TEST_F(DBTest, OperationsAfterCloseAreRejected) {
  auto db = OpenOrDie();
  ASSERT_TRUE(db->Close().ok());

  std::string value;
  EXPECT_TRUE(db->Put(WriteOptions{}, "k", "v").IsInvalidArgument());
  EXPECT_TRUE(db->Get(ReadOptions{}, "k", &value).IsInvalidArgument());
  EXPECT_TRUE(db->Close().ok()) << "closing twice must be harmless";
}

TEST_F(DBTest, DataSurvivesCloseAndReopen) {
  {
    auto db = OpenOrDie();
    ASSERT_TRUE(db->Put(WriteOptions{}, "persisted", "yes").ok());
    ASSERT_TRUE(db->Delete(WriteOptions{}, "removed").ok());
    ASSERT_TRUE(db->Put(WriteOptions{}, "removed", "back").ok());
    ASSERT_TRUE(db->Close().ok());
  }

  auto reopened = OpenOrDie();
  std::string value;
  ASSERT_TRUE(reopened->Get(ReadOptions{}, "persisted", &value).ok());
  EXPECT_EQ(value, "yes");
  ASSERT_TRUE(reopened->Get(ReadOptions{}, "removed", &value).ok());
  EXPECT_EQ(value, "back");
}

TEST_F(DBTest, TombstonesSurviveReopen) {
  {
    auto db = OpenOrDie();
    ASSERT_TRUE(db->Put(WriteOptions{}, "k", "v").ok());
    ASSERT_TRUE(db->Delete(WriteOptions{}, "k").ok());
    ASSERT_TRUE(db->Close().ok());
  }

  auto reopened = OpenOrDie();
  std::string value;
  EXPECT_TRUE(reopened->Get(ReadOptions{}, "k", &value).IsNotFound())
      << "a delete that was acknowledged must still be a delete after recovery";
}

TEST_F(DBTest, WritesContinueAcrossSeveralReopens) {
  for (int round = 0; round < 3; ++round) {
    auto db = OpenOrDie();
    ASSERT_TRUE(db->Put(WriteOptions{}, "round-" + std::to_string(round), "x").ok());
    ASSERT_TRUE(db->Close().ok());
  }

  auto db = OpenOrDie();
  std::string value;
  for (int round = 0; round < 3; ++round) {
    EXPECT_TRUE(db->Get(ReadOptions{}, "round-" + std::to_string(round), &value).ok())
        << "round " << round;
  }
}

TEST_F(DBTest, NoSyncPolicyStillPersistsAfterACleanClose) {
  Options no_sync;
  no_sync.sync_policy = SyncPolicy::kNoSync;
  {
    auto db = OpenOrDie(no_sync);
    ASSERT_TRUE(db->Put(WriteOptions{}, "k", "v").ok());
    ASSERT_TRUE(db->Close().ok()) << "Close syncs, so a clean shutdown must not lose data";
  }

  auto reopened = OpenOrDie(no_sync);
  std::string value;
  ASSERT_TRUE(reopened->Get(ReadOptions{}, "k", &value).ok());
  EXPECT_EQ(value, "v");
}

// --- P1 acceptance criteria ---

// "A test writes 10,000 keys, simulates a torn final record by truncating the
// WAL mid-record, reopens, and recovers every complete record."
TEST_F(DBTest, TenThousandKeysSurviveATornFinalRecord) {
  constexpr int kKeys = 10000;
  {
    auto db = OpenOrDie();
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok()) << i;
    }
    ASSERT_TRUE(db->Close().ok());
  }

  uint64_t size_before = 0;
  ASSERT_TRUE(env_.GetFileSize(WalPath(), &size_before).ok());

  // Cut the log in the middle of its final record, which is exactly what a
  // crash mid-append leaves behind.
  const uint64_t cut = size_before - 6;
  ASSERT_TRUE(env_.TruncateFile(WalPath(), cut).ok());

  auto db = OpenOrDie();
  std::string value;

  // Every key except possibly the last must be recovered: that one was being
  // written when the "crash" happened, so it was never acknowledged.
  for (int i = 0; i < kKeys - 1; ++i) {
    ASSERT_TRUE(db->Get(ReadOptions{}, NumberedKey(i), &value).ok()) << "lost key " << i;
    EXPECT_EQ(value, NumberedValue(i));
  }

  // And the database must be usable afterwards, appending onto the truncated
  // log rather than leaving a half record in the middle.
  ASSERT_TRUE(db->Put(WriteOptions{}, "after-recovery", "ok").ok());
  ASSERT_TRUE(db->Get(ReadOptions{}, "after-recovery", &value).ok());
  EXPECT_EQ(value, "ok");
  ASSERT_TRUE(db->Close().ok());

  // One more reopen proves the post-recovery append is itself replayable.
  auto again = OpenOrDie();
  ASSERT_TRUE(again->Get(ReadOptions{}, "after-recovery", &value).ok());
  EXPECT_EQ(value, "ok");
  ASSERT_TRUE(again->Get(ReadOptions{}, NumberedKey(0), &value).ok());
}

// "Corrupting one byte in the middle record is detected as a checksum failure."
TEST_F(DBTest, OneCorruptByteInAMiddleRecordIsDetected) {
  constexpr int kKeys = 100;
  {
    auto db = OpenOrDie();
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(db->Close().ok());
  }

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(WalPath(), &contents).ok());
  ASSERT_FALSE(contents.empty());

  // Flip a single bit in the middle of the log, far from either end.
  const size_t target = contents.size() / 2;
  contents[target] = static_cast<char>(contents[target] ^ 0x01);
  ASSERT_TRUE(env_.WriteFile(WalPath(), contents).ok());

  std::unique_ptr<DB> db;
  const Status status = Open(&db);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
  EXPECT_EQ(db, nullptr) << "a corrupt database must not open silently";
}

// A failed open must not leak the LOCK file. If it did, a corrupt database
// could never be retried or repaired without killing the process.
TEST_F(DBTest, AFailedOpenReleasesTheLock) {
  // Several records, so that damaging an early one leaves valid records after
  // it. With a single record there is nothing after the damage, which is
  // deliberately treated as a torn tail instead.
  {
    auto db = OpenOrDie();
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(db->Close().ok());
  }

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(WalPath(), &contents).ok());
  const std::string good_contents = contents;

  // Corrupt the middle so the open fails.
  const size_t target = contents.size() / 2;
  contents[target] = static_cast<char>(contents[target] ^ 0x01);
  ASSERT_TRUE(env_.WriteFile(WalPath(), contents).ok());

  std::unique_ptr<DB> failed;
  ASSERT_TRUE(Open(&failed).IsCorruption());
  ASSERT_EQ(failed, nullptr);

  // Restoring the log must now let the database open: the lock from the failed
  // attempt has to be gone.
  ASSERT_TRUE(env_.WriteFile(WalPath(), good_contents).ok());
  std::unique_ptr<DB> reopened;
  const Status status = Open(&reopened);
  ASSERT_TRUE(status.ok()) << status.ToString();

  std::string value;
  ASSERT_TRUE(reopened->Get(ReadOptions{}, NumberedKey(0), &value).ok());
  EXPECT_EQ(value, NumberedValue(0));
}

// Two consecutive failed opens must each clean up after themselves.
TEST_F(DBTest, RepeatedFailedOpensDoNotAccumulateLocks) {
  {
    auto db = OpenOrDie();
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
    }
    ASSERT_TRUE(db->Close().ok());
  }

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(WalPath(), &contents).ok());
  const size_t target = contents.size() / 2;
  contents[target] = static_cast<char>(contents[target] ^ 0x01);
  ASSERT_TRUE(env_.WriteFile(WalPath(), contents).ok());

  for (int attempt = 0; attempt < 3; ++attempt) {
    std::unique_ptr<DB> failed;
    EXPECT_TRUE(Open(&failed).IsCorruption()) << "attempt " << attempt;
  }
}

// The inherent limit of recovering from a log alone: a damaged FINAL record is
// byte-for-byte indistinguishable from a write that was interrupted, so it is
// truncated rather than reported. That silently drops an acknowledged write if
// the cause was disk corruption rather than a crash.
//
// P1 cannot do better -- nothing in the log says how many records there should
// be. The MANIFEST in P3 records the last flushed sequence number, which is the
// information needed to notice the difference. This test pins the current
// behavior so the improvement is a deliberate change rather than a surprise.
TEST_F(DBTest, ADamagedFinalRecordIsTruncatedNotReported) {
  {
    auto db = OpenOrDie();
    ASSERT_TRUE(db->Put(WriteOptions{}, "only-key", "only-value").ok());
    ASSERT_TRUE(db->Close().ok());
  }

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(WalPath(), &contents).ok());
  const size_t target = contents.size() - 2;  // Inside the one and only record.
  contents[target] = static_cast<char>(contents[target] ^ 0x01);
  ASSERT_TRUE(env_.WriteFile(WalPath(), contents).ok());

  auto db = OpenOrDie();
  std::string value;
  EXPECT_TRUE(db->Get(ReadOptions{}, "only-key", &value).IsNotFound())
      << "documented limitation: the damaged last record is dropped, not reported";
}

TEST_F(DBTest, ManyKeysRoundTripWithOverwritesAndDeletes) {
  constexpr int kKeys = 5000;
  auto db = OpenOrDie();

  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), NumberedValue(i)).ok());
  }
  // Overwrite every third key and delete every fifth.
  for (int i = 0; i < kKeys; i += 3) {
    ASSERT_TRUE(db->Put(WriteOptions{}, NumberedKey(i), "overwritten").ok());
  }
  for (int i = 0; i < kKeys; i += 5) {
    ASSERT_TRUE(db->Delete(WriteOptions{}, NumberedKey(i)).ok());
  }
  ASSERT_TRUE(db->Close().ok());
  db.reset();

  auto reopened = OpenOrDie();
  std::string value;
  for (int i = 0; i < kKeys; ++i) {
    const Status status = reopened->Get(ReadOptions{}, NumberedKey(i), &value);
    if (i % 5 == 0) {
      EXPECT_TRUE(status.IsNotFound()) << "key " << i << " was deleted";
    } else if (i % 3 == 0) {
      ASSERT_TRUE(status.ok()) << "key " << i;
      EXPECT_EQ(value, "overwritten") << "key " << i;
    } else {
      ASSERT_TRUE(status.ok()) << "key " << i;
      EXPECT_EQ(value, NumberedValue(i)) << "key " << i;
    }
  }
}

}  // namespace
}  // namespace stratakv
