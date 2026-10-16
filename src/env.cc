#include "env.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stratakv/status.h"

namespace stratakv {
namespace {

constexpr mode_t kFileMode = 0644;
constexpr mode_t kDirMode = 0755;

Status PosixError(const std::string& path, int err) {
  return Status::IOError(path, std::strerror(err));
}

// Test-only fault injection state. Atomic so a test may arm it from one thread
// while the background thread reads it from another.
std::mutex& InjectionMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& InjectionSubstring() {
  static std::string substring;
  return substring;
}

int& InjectionRemaining() {
  static int remaining = 0;
  return remaining;
}

// True when this path should be failed, consuming one of the armed failures.
bool ShouldInjectFailure(const std::string& path) {
  const std::lock_guard<std::mutex> guard(InjectionMutex());
  if (InjectionRemaining() <= 0 || InjectionSubstring().empty()) return false;
  if (path.find(InjectionSubstring()) == std::string::npos) return false;
  --InjectionRemaining();
  return true;
}

// Writes every byte, looping past short writes and retrying when a signal
// interrupts the call. A partial write is not an error the engine can tolerate
// silently: half a WAL record on disk must still be a torn record, never a
// record the engine believes it wrote.
Status WriteFully(int fd, const uint8_t* data, size_t n, const std::string& path) {
  size_t written = 0;
  while (written < n) {
    const ssize_t result = ::write(fd, data + written, n - written);
    if (result < 0) {
      if (errno == EINTR) continue;
      return PosixError(path, errno);
    }
    written += static_cast<size_t>(result);
  }
  return Status::OK();
}

// The one place that decides what "on disk" means.
//
// On macOS, fsync() hands data to the drive but does not ask the drive to empty
// its own volatile write cache, so a power cut can still lose it. F_FULLFSYNC
// does ask. Some filesystems and network mounts do not implement F_FULLFSYNC,
// so a failure there falls back to plain fsync rather than failing the write.
Status FullSyncFd(int fd, const std::string& path) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::OK();
#endif
  if (::fsync(fd) != 0) return PosixError(path, errno);
  return Status::OK();
}

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(int fd, std::string path, uint64_t size = 0)
      : fd_(fd), path_(std::move(path)), size_(size) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) ::close(fd_);  // Nothing useful to do with a close error here.
  }

  Status Append(std::span<const uint8_t> data) override {
    if (!sync_error_.ok()) return sync_error_;
    if (fd_ < 0) return Status::IOError(path_, "append to a closed file");
    if (auto s = WriteFully(fd_, data.data(), data.size(), path_); !s.ok()) return s;
    size_ += data.size();
    return Status::OK();
  }

  // A failed fsync is permanent, not retryable. On Linux the kernel may drop
  // the dirty pages and clear the error, so a second fsync can report success
  // while the data is gone -- the bug Postgres named "fsyncgate" in 2018. The
  // error is therefore remembered and returned from every later call, so the
  // engine can never acknowledge a write on top of a sync that failed.
  Status Sync() override {
    if (!sync_error_.ok()) return sync_error_;
    if (fd_ < 0) return Status::IOError(path_, "sync of a closed file");
    if (auto s = FullSyncFd(fd_, path_); !s.ok()) {
      sync_error_ = s;
      return s;
    }
    return Status::OK();
  }

  Status Truncate(uint64_t size) override {
    if (!sync_error_.ok()) return sync_error_;
    if (fd_ < 0) return Status::IOError(path_, "truncate of a closed file");
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) return PosixError(path_, errno);
    size_ = size;
    return Status::OK();
  }

  Status Close() override {
    if (fd_ < 0) return Status::OK();
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) return PosixError(path_, errno);
    return sync_error_;
  }

  uint64_t Size() const override { return size_; }

 private:
  int fd_;
  std::string path_;
  uint64_t size_;
  Status sync_error_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(int fd, std::string path, uint64_t size)
      : fd_(fd), path_(std::move(path)), size_(size) {}

  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) ::close(fd_);
  }

  // pread does not use or move a file cursor, so concurrent calls on one
  // descriptor are safe and need no lock.
  Status ReadAt(uint64_t offset, uint8_t* dst, size_t n, size_t* got) const override {
    size_t total = 0;
    while (total < n) {
      const ssize_t result =
          ::pread(fd_, dst + total, n - total, static_cast<off_t>(offset + total));
      if (result < 0) {
        if (errno == EINTR) continue;
        *got = total;
        return PosixError(path_, errno);
      }
      if (result == 0) break;  // End of file.
      total += static_cast<size_t>(result);
    }
    *got = total;
    return Status::OK();
  }

  uint64_t Size() const override { return size_; }

 private:
  int fd_;
  std::string path_;
  uint64_t size_;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

  // Closing the descriptor would release the flock on its own; unlocking
  // explicitly makes the intent obvious to a reader.
  ~PosixFileLock() override {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
    }
  }

 private:
  int fd_;
  std::string path_;
};

class PosixEnv final : public Env {
 public:
  Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override {
    if (ShouldInjectFailure(path)) {
      return Status::IOError(path, "injected failure (test only)");
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
    if (fd < 0) return PosixError(path, errno);
    *out = std::make_unique<PosixWritableFile>(fd, path);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, kFileMode);
    if (fd < 0) return PosixError(path, errno);

    // Seed the in-memory size from the file on disk so Size() stays truthful
    // across a reopen.
    struct ::stat info{};
    if (::fstat(fd, &info) != 0) {
      const Status s = PosixError(path, errno);
      ::close(fd);
      return s;
    }
    *out = std::make_unique<PosixWritableFile>(fd, path, static_cast<uint64_t>(info.st_size));
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* out) override {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return PosixError(path, errno);

    struct ::stat info{};
    if (::fstat(fd, &info) != 0) {
      const Status s = PosixError(path, errno);
      ::close(fd);
      return s;
    }
    *out = std::make_unique<PosixRandomAccessFile>(fd, path, static_cast<uint64_t>(info.st_size));
    return Status::OK();
  }

  Status TruncateFile(const std::string& path, uint64_t size) override {
    if (::truncate(path.c_str(), static_cast<off_t>(size)) != 0) {
      return PosixError(path, errno);
    }
    return Status::OK();
  }

  Status WriteFile(const std::string& path, std::string_view contents) override {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kFileMode);
    if (fd < 0) return PosixError(path, errno);
    Status s =
        WriteFully(fd, reinterpret_cast<const uint8_t*>(contents.data()), contents.size(), path);
    if (::close(fd) != 0 && s.ok()) return PosixError(path, errno);
    return s;
  }

  Status ReadFileToString(const std::string& path, std::string* out) override {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return PosixError(path, errno);
    out->clear();
    std::array<char, 8192> buffer{};
    while (true) {
      const ssize_t result = ::read(fd, buffer.data(), buffer.size());
      if (result < 0) {
        if (errno == EINTR) continue;
        Status s = PosixError(path, errno);
        ::close(fd);
        return s;
      }
      if (result == 0) break;
      out->append(buffer.data(), static_cast<size_t>(result));
    }
    ::close(fd);
    return Status::OK();
  }

  // Opened for writing because POSIX permits fsync to fail on a descriptor that
  // is not open for writing, and the only files the engine syncs by path are
  // ones it just wrote.
  Status SyncFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return PosixError(path, errno);
    const Status s = FullSyncFd(fd, path);
    ::close(fd);
    return s;
  }

  // A directory can only be opened read-only, which is enough: the data being
  // flushed is the directory's own list of names, not file contents.
  Status SyncDir(const std::string& dir) override {
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return PosixError(dir, errno);
    const Status s = FullSyncFd(fd, dir);
    ::close(fd);
    return s;
  }

  Status Rename(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) {
      return PosixError(from + " -> " + to, errno);
    }
    return Status::OK();
  }

  Status RemoveFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) return PosixError(path, errno);
    return Status::OK();
  }

  Status CreateDirIfMissing(const std::string& path) override {
    if (::mkdir(path.c_str(), kDirMode) == 0) return Status::OK();
    if (errno == EEXIST) {
      struct ::stat info{};
      if (::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return Status::OK();
      return Status::IOError(path, "exists and is not a directory");
    }
    return PosixError(path, errno);
  }

  bool FileExists(const std::string& path) override { return ::access(path.c_str(), F_OK) == 0; }

  Status GetChildren(const std::string& directory, std::vector<std::string>* result) override {
    result->clear();
    ::DIR* handle = ::opendir(directory.c_str());
    if (handle == nullptr) return PosixError(directory, errno);
    while (const ::dirent* entry = ::readdir(handle)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      result->push_back(name);
    }
    ::closedir(handle);
    return Status::OK();
  }

  Status GetFileSize(const std::string& path, uint64_t* size) override {
    struct ::stat info{};
    if (::stat(path.c_str(), &info) != 0) return PosixError(path, errno);
    *size = static_cast<uint64_t>(info.st_size);
    return Status::OK();
  }

  // LOCK_NB makes this fail immediately instead of waiting, so opening a
  // database that another process already has open reports an error rather
  // than hanging. The lock belongs to the open file description, so a second
  // attempt fails even from within the same process.
  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* out) override {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, kFileMode);
    if (fd < 0) return PosixError(path, errno);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int err = errno;
      ::close(fd);
      if (err == EWOULDBLOCK) {
        return Status::IOError(path, "already locked by another process");
      }
      return PosixError(path, err);
    }
    *out = std::make_unique<PosixFileLock>(fd, path);
    return Status::OK();
  }
};

}  // namespace

void Env::SetFailureInjectionForTesting(std::string_view path_substring, int count) {
  const std::lock_guard<std::mutex> guard(InjectionMutex());
  InjectionSubstring().assign(path_substring);
  InjectionRemaining() = count;
}

Env& Env::Default() {
  // Function-local static: constructed once, on first use, thread-safely.
  static PosixEnv env;
  return env;
}

}  // namespace stratakv
