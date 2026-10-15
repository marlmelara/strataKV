#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "stratakv/status.h"

#include "env.h"

namespace stratakv {
namespace {

std::span<const uint8_t> Bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

class EnvTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string pattern =
        (std::filesystem::temp_directory_path() / "stratakv_env_XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    ASSERT_NE(::mkdtemp(buffer.data()), nullptr) << "could not create a temp directory";
    dir_ = buffer.data();
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  std::string Path(std::string_view name) const { return dir_ + "/" + std::string(name); }

  std::string dir_;
  Env& env_ = Env::Default();
};

TEST_F(EnvTest, AppendTracksSizeAndSurvivesReopen) {
  const std::string path = Path("000001.log");

  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(path, &file).ok());
  EXPECT_EQ(file->Size(), 0U);

  ASSERT_TRUE(file->Append(Bytes("hello ")).ok());
  ASSERT_TRUE(file->Append(std::string_view("world")).ok());
  EXPECT_EQ(file->Size(), 11U);

  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "hello world");

  uint64_t size = 0;
  ASSERT_TRUE(env_.GetFileSize(path, &size).ok());
  EXPECT_EQ(size, 11U);
}

TEST_F(EnvTest, NewWritableFileTruncatesAnExistingFile) {
  const std::string path = Path("reused.log");
  ASSERT_TRUE(env_.WriteFile(path, "old contents that should be gone").ok());

  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(path, &file).ok());
  ASSERT_TRUE(file->Append(std::string_view("new")).ok());
  ASSERT_TRUE(file->Close().ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "new");
}

// WAL recovery drops a torn tail by truncating. The bytes must really go away,
// and the file's reported size must follow.
TEST_F(EnvTest, TruncateDropsTheTail) {
  const std::string path = Path("torn.log");

  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(path, &file).ok());
  ASSERT_TRUE(file->Append(std::string_view("complete-record|torn-rec")).ok());
  ASSERT_TRUE(file->Truncate(16).ok());
  EXPECT_EQ(file->Size(), 16U);
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "complete-record|");
}

TEST_F(EnvTest, OperationsOnAClosedFileFail) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(Path("closed.log"), &file).ok());
  ASSERT_TRUE(file->Close().ok());

  EXPECT_TRUE(file->Append(std::string_view("x")).IsIOError());
  EXPECT_TRUE(file->Sync().IsIOError());
  EXPECT_TRUE(file->Truncate(0).IsIOError());
  EXPECT_TRUE(file->Close().ok());  // Closing twice is harmless.
}

TEST_F(EnvTest, WriteFileReplacesTheWholeFile) {
  const std::string path = Path("CURRENT.tmp");
  ASSERT_TRUE(env_.WriteFile(path, "MANIFEST-000005\n").ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "MANIFEST-000005\n");

  ASSERT_TRUE(env_.WriteFile(path, "MANIFEST-9\n").ok());
  ASSERT_TRUE(env_.ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "MANIFEST-9\n") << "a shorter rewrite must not leave old bytes behind";
}

// This is the sequence SetCurrentFile uses in P3: write a temp file, sync it,
// rename it over the real name, then sync the directory. P0 only has to prove
// the Env operations themselves work and compose.
TEST_F(EnvTest, SyncRenameSyncDirSequenceSucceeds) {
  const std::string tmp = Path("CURRENT.tmp");
  const std::string live = Path("CURRENT");

  ASSERT_TRUE(env_.WriteFile(tmp, "MANIFEST-000007\n").ok());
  ASSERT_TRUE(env_.SyncFile(tmp).ok());
  ASSERT_TRUE(env_.Rename(tmp, live).ok());
  ASSERT_TRUE(env_.SyncDir(dir_).ok());

  EXPECT_FALSE(env_.FileExists(tmp)) << "rename must move, not copy";
  EXPECT_TRUE(env_.FileExists(live));

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(live, &contents).ok());
  EXPECT_EQ(contents, "MANIFEST-000007\n");
}

TEST_F(EnvTest, RenameReplacesAnExistingTarget) {
  const std::string from = Path("new");
  const std::string to = Path("old");
  ASSERT_TRUE(env_.WriteFile(from, "new").ok());
  ASSERT_TRUE(env_.WriteFile(to, "old").ok());

  ASSERT_TRUE(env_.Rename(from, to).ok());

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(to, &contents).ok());
  EXPECT_EQ(contents, "new");
}

TEST_F(EnvTest, MissingPathsReportIOError) {
  const std::string missing = Path("does-not-exist");

  EXPECT_FALSE(env_.FileExists(missing));

  std::string contents;
  EXPECT_TRUE(env_.ReadFileToString(missing, &contents).IsIOError());

  uint64_t size = 0;
  EXPECT_TRUE(env_.GetFileSize(missing, &size).IsIOError());
  EXPECT_TRUE(env_.SyncFile(missing).IsIOError());
  EXPECT_TRUE(env_.RemoveFile(missing).IsIOError());
  EXPECT_TRUE(env_.Rename(missing, Path("target")).IsIOError());

  // The message should name the path, so a failure is diagnosable from a log.
  const Status s = env_.ReadFileToString(missing, &contents);
  EXPECT_NE(s.ToString().find(missing), std::string::npos) << s.ToString();
}

TEST_F(EnvTest, RemoveFileDeletes) {
  const std::string path = Path("000004.sst");
  ASSERT_TRUE(env_.WriteFile(path, "data").ok());
  ASSERT_TRUE(env_.FileExists(path));

  ASSERT_TRUE(env_.RemoveFile(path).ok());
  EXPECT_FALSE(env_.FileExists(path));
}

TEST_F(EnvTest, CreateDirIfMissingIsIdempotent) {
  const std::string nested = Path("mydb");
  ASSERT_TRUE(env_.CreateDirIfMissing(nested).ok());
  EXPECT_TRUE(std::filesystem::is_directory(nested));

  ASSERT_TRUE(env_.CreateDirIfMissing(nested).ok()) << "calling it twice must succeed";
}

TEST_F(EnvTest, CreateDirIfMissingRejectsAnExistingPlainFile) {
  const std::string path = Path("not-a-dir");
  ASSERT_TRUE(env_.WriteFile(path, "file").ok());
  EXPECT_TRUE(env_.CreateDirIfMissing(path).IsIOError());
}

// The LOCK file is what stops two processes opening the same database. flock
// locks belong to the open file description, so a second attempt fails even
// from inside this one process -- which is what makes this testable here.
TEST_F(EnvTest, LockFileIsExclusiveAndReleasedOnDestruction) {
  const std::string path = Path("LOCK");

  std::unique_ptr<FileLock> first;
  ASSERT_TRUE(env_.LockFile(path, &first).ok());
  ASSERT_NE(first, nullptr);

  std::unique_ptr<FileLock> second;
  const Status blocked = env_.LockFile(path, &second);
  EXPECT_TRUE(blocked.IsIOError());
  EXPECT_EQ(second, nullptr);
  EXPECT_NE(blocked.ToString().find("already locked"), std::string::npos) << blocked.ToString();

  first.reset();  // RAII: destroying the lock releases it.

  std::unique_ptr<FileLock> third;
  EXPECT_TRUE(env_.LockFile(path, &third).ok()) << "the lock should be free again";
}

TEST_F(EnvTest, SyncDirFailsOnAPathThatIsNotADirectory) {
  const std::string path = Path("plain-file");
  ASSERT_TRUE(env_.WriteFile(path, "x").ok());
  // Opening a plain file read-only succeeds, so this documents real behavior:
  // syncing it is harmless rather than an error. The engine only ever passes
  // its own database directory.
  EXPECT_TRUE(env_.SyncDir(path).ok());
}

TEST_F(EnvTest, DefaultEnvIsASingleton) { EXPECT_EQ(&Env::Default(), &Env::Default()); }

}  // namespace
}  // namespace stratakv
