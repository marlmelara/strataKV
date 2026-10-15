#include <gtest/gtest.h>

#include "crash_point.h"

namespace stratakv::crash {
namespace {

// Restores the override so one test cannot affect another.
class CrashPointTest : public ::testing::Test {
 protected:
  void TearDown() override { SetArmedPointForTesting(nullptr); }
};

TEST_F(CrashPointTest, NothingIsArmedByDefault) {
  // The test runner is not started with STRATAKV_CRASH_AT set, so no crash
  // point should match -- otherwise every test below would be meaningless.
  EXPECT_FALSE(IsArmed("write_after_wal_sync"));
  EXPECT_FALSE(IsArmed("flush_after_sst_write"));
}

TEST_F(CrashPointTest, OnlyTheArmedNameMatches) {
  SetArmedPointForTesting("write_after_wal_sync");

  EXPECT_TRUE(IsArmed("write_after_wal_sync"));
  EXPECT_FALSE(IsArmed("write_before_wal"));
  EXPECT_FALSE(IsArmed("write_after_wal_syn"));    // A prefix must not match.
  EXPECT_FALSE(IsArmed("write_after_wal_sync2"));  // Nor an extension.
}

TEST_F(CrashPointTest, ClearingTheOverrideDisarms) {
  SetArmedPointForTesting("write_before_wal");
  ASSERT_TRUE(IsArmed("write_before_wal"));

  SetArmedPointForTesting(nullptr);
  EXPECT_FALSE(IsArmed("write_before_wal"));
}

TEST_F(CrashPointTest, AnUnarmedCrashPointDoesNothing) {
  SetArmedPointForTesting("some_other_point");
  CrashPoint("write_after_wal_sync");  // Must simply return.
  SUCCEED();
}

// The framework's whole purpose: the process must really die, immediately, with
// status 1. A death test runs this in a forked child so the suite survives.
TEST_F(CrashPointTest, AnArmedCrashPointExitsImmediatelyWithStatusOne) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        SetArmedPointForTesting("armed_point");
        CrashPoint("armed_point");
        // Reached only if the crash point failed to fire.
        std::_Exit(99);
      },
      ::testing::ExitedWithCode(1), "");
}

}  // namespace
}  // namespace stratakv::crash
