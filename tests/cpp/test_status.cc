#include <string>

#include <gtest/gtest.h>

#include "stratakv/status.h"

namespace stratakv {
namespace {

TEST(Status, DefaultConstructedIsOk) {
  const Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_TRUE(s.message().empty());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(Status, OkFactoryMatchesDefault) {
  const Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(Status, EachCodeIsDistinguishable) {
  const Status not_found = Status::NotFound("key");
  EXPECT_FALSE(not_found.ok());
  EXPECT_TRUE(not_found.IsNotFound());
  EXPECT_FALSE(not_found.IsCorruption());
  EXPECT_FALSE(not_found.IsIOError());
  EXPECT_FALSE(not_found.IsInvalidArgument());

  const Status corruption = Status::Corruption("wal record 7");
  EXPECT_TRUE(corruption.IsCorruption());
  EXPECT_FALSE(corruption.IsIOError());

  const Status io = Status::IOError("/tmp/db/CURRENT");
  EXPECT_TRUE(io.IsIOError());
  EXPECT_FALSE(io.IsNotFound());

  const Status invalid = Status::InvalidArgument("empty key");
  EXPECT_TRUE(invalid.IsInvalidArgument());
  EXPECT_FALSE(invalid.IsCorruption());
}

TEST(Status, ToStringNamesTheCodeAndJoinsContextWithDetail) {
  EXPECT_EQ(Status::IOError("/tmp/db/CURRENT", "No such file or directory").ToString(),
            "IOError: /tmp/db/CURRENT: No such file or directory");
  EXPECT_EQ(Status::NotFound("key").ToString(), "NotFound: key");
  EXPECT_EQ(Status::Corruption("wal", "checksum mismatch").ToString(),
            "Corruption: wal: checksum mismatch");
  EXPECT_EQ(Status::InvalidArgument("", "key must not be empty").ToString(),
            "InvalidArgument: key must not be empty");
}

TEST(Status, MessageExcludesTheCodeName) {
  const Status s = Status::IOError("/tmp/x", "Permission denied");
  EXPECT_EQ(s.message(), "/tmp/x: Permission denied");
}

// Status owns its message, so it stays valid after the strings that built it
// are gone. Returning one up the stack must never dangle.
TEST(Status, OwnsItsMessageAfterSourcesAreDestroyed) {
  Status s;
  {
    const std::string path = "/tmp/a/transient/path";
    const std::string reason = "Input/output error";
    s = Status::IOError(path, reason);
  }
  EXPECT_EQ(s.ToString(), "IOError: /tmp/a/transient/path: Input/output error");
}

TEST(Status, IsCopyableAndAssignable) {
  const Status original = Status::Corruption("sstable footer", "bad magic");
  const Status copy = original;  // NOLINT(performance-unnecessary-copy-*)
  EXPECT_EQ(copy.ToString(), original.ToString());

  Status assigned;
  assigned = original;
  EXPECT_TRUE(assigned.IsCorruption());
  EXPECT_EQ(assigned.ToString(), original.ToString());
}

}  // namespace
}  // namespace stratakv
