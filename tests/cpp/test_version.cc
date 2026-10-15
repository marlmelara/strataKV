#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "test_util.h"
#include "version.h"

namespace stratakv {
namespace {

using testing::TempDir;

std::string Internal(std::string_view user_key, uint64_t sequence) {
  std::string out;
  AppendInternalKey(&out, user_key, sequence, ValueType::kTypeValue);
  return out;
}

FileMetaData MakeFile(uint64_t number, std::string_view smallest, std::string_view largest) {
  FileMetaData file;
  file.number = number;
  file.file_size = 1234;
  file.smallest = Internal(smallest, 10);
  file.largest = Internal(largest, 20);
  return file;
}

TEST(VersionEdit, RoundTripsEveryField) {
  VersionEdit edit;
  edit.SetLogNumber(7);
  edit.SetNextFileNumber(42);
  edit.SetLastSequence(999);
  edit.AddFile(0, MakeFile(11, "apple", "banana"));
  edit.AddFile(1, MakeFile(12, "cherry", "date"));
  edit.RemoveFile(0, 5);
  edit.RemoveFile(1, 6);

  std::string encoded;
  edit.EncodeTo(&encoded);

  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());

  EXPECT_TRUE(decoded.has_log_number());
  EXPECT_EQ(decoded.log_number(), 7U);
  EXPECT_EQ(decoded.next_file_number(), 42U);
  EXPECT_EQ(decoded.last_sequence(), 999U);

  ASSERT_EQ(decoded.added_files().size(), 2U);
  EXPECT_EQ(decoded.added_files()[0].first, 0);
  EXPECT_EQ(decoded.added_files()[0].second.number, 11U);
  EXPECT_EQ(ExtractUserKey(decoded.added_files()[0].second.smallest), "apple");
  EXPECT_EQ(ExtractUserKey(decoded.added_files()[0].second.largest), "banana");
  EXPECT_EQ(decoded.added_files()[1].first, 1);
  EXPECT_EQ(decoded.added_files()[1].second.number, 12U);

  ASSERT_EQ(decoded.removed_files().size(), 2U);
  EXPECT_EQ(decoded.removed_files()[0], std::make_pair(0, uint64_t{5}));
  EXPECT_EQ(decoded.removed_files()[1], std::make_pair(1, uint64_t{6}));
}

TEST(VersionEdit, AnEmptyEditRoundTrips) {
  VersionEdit edit;
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_TRUE(encoded.empty());

  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  EXPECT_FALSE(decoded.has_log_number());
  EXPECT_TRUE(decoded.added_files().empty());
}

// Every field comes off disk, so each decode step has to be checked.
TEST(VersionEdit, RejectsTruncatedInput) {
  VersionEdit edit;
  edit.SetLogNumber(7);
  edit.SetLastSequence(999);
  edit.AddFile(0, MakeFile(11, "apple", "banana"));

  std::string encoded;
  edit.EncodeTo(&encoded);

  for (size_t length = 1; length < encoded.size(); ++length) {
    VersionEdit decoded;
    const Status status = decoded.DecodeFrom(std::string_view(encoded).substr(0, length));
    // A prefix either stops on a field boundary (valid, just fewer fields) or
    // is refused. What must never happen is a decode that reads past the end.
    if (!status.ok()) EXPECT_TRUE(status.IsCorruption()) << status.ToString();
  }
}

TEST(VersionEdit, RejectsAnUnknownTag) {
  std::string encoded;
  PutVarint32(&encoded, 99);  // Not a tag this version knows.
  VersionEdit decoded;
  EXPECT_TRUE(decoded.DecodeFrom(encoded).IsCorruption());
}

TEST(VersionEdit, RejectsALevelOutOfRange) {
  std::string encoded;
  PutVarint32(&encoded, 5);  // kAddedFile
  PutVarint32(&encoded, 7);  // Level 7 does not exist.
  PutFixed64(&encoded, 1);
  PutFixed64(&encoded, 1);
  PutLengthPrefixed(&encoded, Internal("a", 1));
  PutLengthPrefixed(&encoded, Internal("b", 1));

  VersionEdit decoded;
  const Status status = decoded.DecodeFrom(encoded);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
}

TEST(VersionEdit, RejectsAKeyRangeThatIsNotInternalKeys) {
  std::string encoded;
  PutVarint32(&encoded, 5);  // kAddedFile
  PutVarint32(&encoded, 0);
  PutFixed64(&encoded, 1);
  PutFixed64(&encoded, 1);
  PutLengthPrefixed(&encoded, "short");  // Too short to carry a trailer.
  PutLengthPrefixed(&encoded, "alsoshort");

  VersionEdit decoded;
  EXPECT_TRUE(decoded.DecodeFrom(encoded).IsCorruption());
}

class CurrentFileTest : public ::testing::Test {
 protected:
  TempDir dir_;
  Env& env_ = Env::Default();
};

TEST_F(CurrentFileTest, PointsAtTheManifestAndLeavesNoTemporaryBehind) {
  ASSERT_TRUE(SetCurrentFile(env_, dir_.path(), 5).ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(dir_.Child("CURRENT"), &contents).ok());
  EXPECT_EQ(contents, "MANIFEST-000005\n");

  EXPECT_FALSE(env_.FileExists(dir_.Child("CURRENT.tmp")))
      << "the rename must move the temporary file, not copy it";
}

TEST_F(CurrentFileTest, ReplacingCurrentIsAtomicFromAReaderPointOfView) {
  ASSERT_TRUE(SetCurrentFile(env_, dir_.path(), 5).ok());
  ASSERT_TRUE(SetCurrentFile(env_, dir_.path(), 6).ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(dir_.Child("CURRENT"), &contents).ok());
  EXPECT_EQ(contents, "MANIFEST-000006\n") << "a replacement must be complete, never partial";
}

TEST_F(CurrentFileTest, TheNameItWritesParsesBackAsAManifest) {
  ASSERT_TRUE(SetCurrentFile(env_, dir_.path(), 12345).ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(dir_.Child("CURRENT"), &contents).ok());
  while (!contents.empty() && contents.back() == '\n') contents.pop_back();

  uint64_t number = 0;
  FileType type{};
  ASSERT_TRUE(ParseFileName(contents, &number, &type)) << contents;
  EXPECT_EQ(number, 12345U);
  EXPECT_TRUE(type == FileType::kManifest);
}

class VersionSetTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(env_.CreateDirIfMissing(dir_.path()).ok()); }

  TempDir dir_;
  Env& env_ = Env::Default();
  Options options_;
};

TEST_F(VersionSetTest, AFreshDatabaseWritesAManifestAndCurrent) {
  VersionSet versions(env_, dir_.path(), options_);
  bool created = false;
  ASSERT_TRUE(versions.Recover(&created).ok());

  EXPECT_TRUE(created);
  EXPECT_TRUE(env_.FileExists(dir_.Child("CURRENT")));
  EXPECT_TRUE(env_.FileExists(ManifestFileName(dir_.path(), versions.manifest_number())));
  EXPECT_TRUE(versions.current()->files(0).empty());
}

TEST_F(VersionSetTest, StateSurvivesAReopen) {
  {
    VersionSet versions(env_, dir_.path(), options_);
    bool created = false;
    ASSERT_TRUE(versions.Recover(&created).ok());
    versions.set_last_sequence(4242);
    versions.set_log_number(77);

    VersionEdit edit;
    edit.SetLogNumber(77);
    ASSERT_TRUE(versions.LogAndApply(&edit).ok());
  }

  VersionSet reopened(env_, dir_.path(), options_);
  bool created = true;
  ASSERT_TRUE(reopened.Recover(&created).ok());
  EXPECT_FALSE(created) << "an existing database must be recovered, not recreated";
  EXPECT_EQ(reopened.last_sequence(), 4242U);
  EXPECT_EQ(reopened.log_number(), 77U);
}

TEST_F(VersionSetTest, RejectsACurrentThatNamesNothingSensible) {
  VersionSet versions(env_, dir_.path(), options_);
  bool created = false;
  ASSERT_TRUE(versions.Recover(&created).ok());

  ASSERT_TRUE(env_.WriteFile(dir_.Child("CURRENT"), "not-a-manifest\n").ok());

  VersionSet broken(env_, dir_.path(), options_);
  bool ignored = false;
  const Status status = broken.Recover(&ignored);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
}

// The MANIFEST is now the authoritative record of what the database consists
// of, so damage to it has to be handled as deliberately as damage to a log.
TEST_F(VersionSetTest, ATornManifestTailIsDroppedNotFatal) {
  {
    VersionSet versions(env_, dir_.path(), options_);
    bool created = false;
    ASSERT_TRUE(versions.Recover(&created).ok());
    // Counter-only edits, so no SSTable has to exist for LogAndApply to accept
    // them -- it validates the files an edit adds before writing the record.
    for (int i = 1; i <= 5; ++i) {
      versions.set_last_sequence(static_cast<uint64_t>(100 * i));
      versions.set_log_number(static_cast<uint64_t>(i));
      VersionEdit edit;
      edit.SetLogNumber(static_cast<uint64_t>(i));
      ASSERT_TRUE(versions.LogAndApply(&edit).ok()) << "edit " << i;
    }
  }

  // Find the MANIFEST CURRENT names and cut its final record in half, which is
  // what a crash mid-append leaves.
  std::string current;
  ASSERT_TRUE(env_.ReadFileToString(dir_.Child("CURRENT"), &current).ok());
  while (!current.empty() && current.back() == '\n') current.pop_back();
  const std::string manifest_path = dir_.Child(current);

  uint64_t size = 0;
  ASSERT_TRUE(env_.GetFileSize(manifest_path, &size).ok());
  ASSERT_GT(size, 8U);
  ASSERT_TRUE(env_.TruncateFile(manifest_path, size - 3).ok());

  // Recovery must drop the torn record and succeed: it was never acknowledged.
  VersionSet reopened(env_, dir_.path(), options_);
  bool created = true;
  const Status status = reopened.Recover(&created);
  ASSERT_TRUE(status.ok()) << "a torn MANIFEST tail must not be fatal: " << status.ToString();

  // The edits before the torn one must have survived, so the state is the
  // fourth edit's, not the fifth's and not empty.
  EXPECT_EQ(reopened.log_number(), 4U) << "the last complete edit should be the recovered state";
  EXPECT_EQ(reopened.last_sequence(), 400U);
}

TEST_F(VersionSetTest, AManifestCurrentNamesButWhichIsMissingIsReported) {
  {
    VersionSet versions(env_, dir_.path(), options_);
    bool created = false;
    ASSERT_TRUE(versions.Recover(&created).ok());
  }

  std::string current;
  ASSERT_TRUE(env_.ReadFileToString(dir_.Child("CURRENT"), &current).ok());
  while (!current.empty() && current.back() == '\n') current.pop_back();
  ASSERT_TRUE(env_.RemoveFile(dir_.Child(current)).ok());

  VersionSet reopened(env_, dir_.path(), options_);
  bool created = true;
  const Status status = reopened.Recover(&created);
  // Either outcome is defensible, but it must be an error rather than a silent
  // empty database: CURRENT promised a MANIFEST that is not there.
  EXPECT_FALSE(status.ok()) << "a missing MANIFEST must not open as an empty database";
}

TEST_F(VersionSetTest, AnEmptyManifestIsReported) {
  {
    VersionSet versions(env_, dir_.path(), options_);
    bool created = false;
    ASSERT_TRUE(versions.Recover(&created).ok());
  }

  std::string current;
  ASSERT_TRUE(env_.ReadFileToString(dir_.Child("CURRENT"), &current).ok());
  while (!current.empty() && current.back() == '\n') current.pop_back();
  ASSERT_TRUE(env_.WriteFile(dir_.Child(current), "").ok());

  VersionSet reopened(env_, dir_.path(), options_);
  bool created = true;
  EXPECT_TRUE(reopened.Recover(&created).IsCorruption());
}

TEST_F(VersionSetTest, FileNumbersNeverRepeat) {
  VersionSet versions(env_, dir_.path(), options_);
  bool created = false;
  ASSERT_TRUE(versions.Recover(&created).ok());

  std::set<uint64_t> seen;
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(seen.insert(versions.NewFileNumber()).second) << "a file number was reused";
  }
  versions.MarkFileNumberUsed(10000);
  EXPECT_GT(versions.NewFileNumber(), 10000U);
}

}  // namespace
}  // namespace stratakv
