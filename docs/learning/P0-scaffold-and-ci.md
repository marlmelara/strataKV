# P0 — Scaffold and CI

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are defined in [GLOSSARY.md](../GLOSSARY.md) or the build spec's Glossary tab; new C++
syntax is explained in [CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

P0 builds no database yet. It builds the ground everything else stands on: a **CMake**
project that compiles the engine four ways (ordinary, optimized, and under two
**sanitizers** that detect memory errors and data races at runtime), and automated checks
that run all of it on Linux and macOS on every push.

Four pieces of real engine code landed. `Status` is how functions report failure — a
returned value, not a thrown exception. `Options` holds configuration. `Env` is the single
doorway to the operating system, so every `fsync` and rename the engine performs sits in
one reviewable file. `crc32c` computes a **checksum**, the fingerprint that later tells a
half-written record from a complete one.

Finally, `pip install .` compiles the C++ and produces an importable `stratakv` package,
proving the whole chain works before there is anything interesting to expose.

## Data flow

There is no `Put` to trace yet — the write-ahead log and memtable arrive in P1. Two traces
are available, and both matter.

### Trace 1: making a file replacement survive a crash

This is the sequence P3 will use to swap the `CURRENT` file, and P0 is where every step it
depends on got implemented and tested. It is the clearest illustration of why durability is
about *ordering*, not about any single call.

Test: `EnvTest.SyncRenameSyncDirSequenceSucceeds` in
[tests/cpp/test_env.cc](../../tests/cpp/test_env.cc).

1. **`env_.WriteFile(tmp, "MANIFEST-000007\n")`** → `PosixEnv::WriteFile` in
   [src/env.cc](../../src/env.cc) opens `CURRENT.tmp` with `O_CREAT | O_TRUNC`, calls
   `WriteFully` to push every byte, then closes it. At this moment the bytes are in the
   OS's **page cache**, not on the drive. A power cut here loses them — which is fine,
   because nothing has been promised yet.
2. **`env_.SyncFile(tmp)`** → opens the file again and calls `FullSyncFd`, which on macOS
   issues `fcntl(fd, F_FULLFSYNC)` to make the drive empty its own write cache. *Now* the
   contents are durable.
3. **`env_.Rename(tmp, live)`** → `::rename`, which the OS performs atomically: any reader
   sees either the old `CURRENT` or the new one, never a blend of the two.
4. **`env_.SyncDir(dir_)`** → opens the *directory* and syncs it. A rename changes the
   directory's own list of names, and that list is data that can sit in cache like any
   other. Without this step the rename itself can disappear in a crash.

Reversing steps 1 and 2 is the classic bug: the rename reaches disk before the contents do,
and the crash leaves `CURRENT` naming a file that is empty.

### Trace 2: `import stratakv` → `stratakv.version()`

1. `pip install .` reads [pyproject.toml](../../pyproject.toml), which names
   **scikit-build-core** as the build backend.
2. scikit-build-core runs CMake with `STRATAKV_BUILD_PYTHON=ON` and
   `STRATAKV_BUILD_TESTS=OFF` — tests off so the wheel build never needs the network to
   fetch GoogleTest.
3. CMake compiles the engine into `libstratakv.a`, then `pybind11_add_module` compiles
   [python/bindings.cc](../../python/bindings.cc) and links the library into a shared
   module `_stratakv`.
4. `install(TARGETS _stratakv DESTINATION stratakv)` places that module *inside* the
   package directory, next to `__init__.py`.
5. `import stratakv` runs [python/stratakv/\_\_init\_\_.py](../../python/stratakv/__init__.py),
   which imports `version` from the compiled `._stratakv`.
6. `stratakv.version()` calls through pybind11 into `stratakv::Version()` in
   [src/version.cc](../../src/version.cc), which returns the string CMake baked in at
   compile time.

The reason the version comes from the compiled module rather than from package metadata:
it reports the build that is *actually loaded*. A stale `.so` left over from an earlier
install would otherwise go unnoticed.

## Annotated real code

### 1. `FullSyncFd` — the one place that decides what "on disk" means

From [src/env.cc](../../src/env.cc):

```cpp
Status FullSyncFd(int fd, const std::string& path) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::OK();
#endif
  if (::fsync(fd) != 0) return PosixError(path, errno);
  return Status::OK();
}
```

- `Status FullSyncFd(...)` — returns a `Status` value rather than throwing. It lives in an
  **anonymous namespace**, so it is private to this one file.
- `int fd` — a **file descriptor**, the OS's integer handle for an open file. Python exposes
  the same number as `f.fileno()`.
- `const std::string& path` — a read-only reference, passed only so an error message can
  name the file. No copy is made.
- `#if defined(__APPLE__)` — a *preprocessor* condition, decided when the code is compiled,
  not when it runs. On Linux the compiler never even sees the line inside. Python's nearest
  equivalent is `if sys.platform == "darwin":`, which is checked every time instead.
- `::fcntl(fd, F_FULLFSYNC)` — the leading `::` means "the global C function." On macOS,
  plain `fsync` hands data to the drive but does not ask the drive to flush its own
  volatile cache; `F_FULLFSYNC` does. Python: `fcntl.fcntl(fd, fcntl.F_FULLFSYNC)`.
- The fall-through is deliberate, not an oversight: some filesystems and network mounts do
  not implement `F_FULLFSYNC` and fail immediately. Falling back to `fsync` is better than
  failing every write on those mounts.
- `PosixError(path, errno)` — wraps the OS error number into `Status::IOError`.
  `std::strerror(errno)` turns it into readable text, like Python's `OSError.strerror`.

**Compared to the learning guide (snippet 12).** The guide writes this logic directly inside
`PosixWritableFile::Sync()`. The real code factors it into a free function because three
callers need it: `WritableFile::Sync`, `Env::SyncFile`, and `Env::SyncDir`. Same logic, one
copy.

### 2. `PosixWritableFile::Sync` — why a failed `fsync` is permanent

```cpp
Status Sync() override {
  if (!sync_error_.ok()) return sync_error_;
  if (fd_ < 0) return Status::IOError(path_, "sync of a closed file");
  if (auto s = FullSyncFd(fd_, path_); !s.ok()) {
    sync_error_ = s;
    return s;
  }
  return Status::OK();
}
```

- `override` — tells the compiler this replaces a virtual function in `WritableFile`. If the
  signature did not match, the build would fail instead of silently creating a new function.
- `if (!sync_error_.ok()) return sync_error_;` — the sticky check. Once a sync has failed,
  every later call fails the same way.
- `if (auto s = FullSyncFd(...); !s.ok())` — declare `s` and test it in one statement; `s`
  exists only inside the `if`. Python 3.8+: `if not (s := full_sync_fd(...)).ok():`.
- `sync_error_ = s;` — remember the failure forever.

**Why.** This is the one place the real code deliberately goes beyond the guide. On Linux a
failed `fsync` may leave the kernel having already discarded the dirty pages *and* cleared
the error flag, so a second `fsync` returns success over data that no longer exists.
Postgres hit this in 2018 — **fsyncgate** — and now treats `fsync` failure as fatal. A
sticky error is how that lesson is encoded here: the engine can never acknowledge a write
on top of a sync that failed.

### 3. `WriteFully` — short writes are not an error, they are the normal case

```cpp
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
```

- `const uint8_t* data` — a pointer to raw bytes. `data + written` is pointer arithmetic:
  "the address `written` bytes further along."
- `ssize_t result` — a *signed* size, because `write` returns `-1` on failure. `size_t` is
  unsigned and could not express that.
- `::write` may accept **fewer bytes than requested** and that is legal POSIX behavior, so
  the loop keeps going until everything is in. Python's `f.write()` hides this for you;
  `os.write()` does not.
- `if (errno == EINTR) continue;` — a signal interrupted the call before it did anything.
  Retrying is correct, not a workaround.
- `static_cast<size_t>(result)` — an explicit conversion from signed to unsigned, safe here
  because `result` is known non-negative at this point. The build uses `-Wsign-conversion`
  and `-Werror`, so an implicit conversion would not compile.

Why it matters for a storage engine: silently writing half a record and reporting success
would mean the engine believes a record exists that was never fully written. A torn record
on disk is fine — recovery handles it — but the engine thinking it wrote one is not.

### 4. `crc32c::Extend` — the checksum, and why it is inverted twice

From [src/crc32c.cc](../../src/crc32c.cc):

```cpp
uint32_t Extend(uint32_t crc, const uint8_t* data, size_t n) {
  uint32_t c = crc ^ 0xFFFFFFFFU;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t index = (c ^ static_cast<uint32_t>(data[i])) & 0xFFU;
    c = kTable[index] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}
```

- `uint32_t` — an unsigned integer of exactly 32 bits. Python ints are unbounded, so there
  is no direct equivalent; `struct.pack("<I", n)` is the closest idea.
- `c = crc ^ 0xFFFFFFFFU` and the matching `^ 0xFFFFFFFFU` on return — the standard pre- and
  post-inversion. Without it, leading zero bytes would not change the result, and a
  truncated record padded with zeros could checksum as valid. The
  `Crc32c.DetectsTruncation` test exists precisely to pin this down.
- `kTable[index] ^ (c >> 8)` — the table lookup. Each entry holds the precomputed
  contribution of one byte, which turns eight bit-operations into one lookup. The table is
  built by a `constexpr` function, so the compiler computes all 256 entries and there is no
  setup cost at runtime.
- `static_cast<uint32_t>(data[i])` — explicit because the warning flags demand it.
- `Extend(Value(a), b)` equals the checksum of `a` followed by `b`. That property is not
  incidental: P1's `WalWriter::AddRecord` checksums the one-byte record type, then continues
  the same checksum over the payload, without copying them into a single buffer.
  `Crc32c.ExtendEqualsChecksumOfConcatenation` guards it.

Python equivalent: there is no `crc32c` in the standard library (`zlib.crc32` is a
*different* polynomial and will not match). A bit-by-bit reference implementation, used to
confirm every value asserted in the tests, lives in
[tools/crc32c_reference.py](../../tools/crc32c_reference.py).

### 5. `PosixEnv::LockFile` — why one database cannot be opened twice

```cpp
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
```

- `std::unique_ptr<FileLock>* out` — the result comes back through an out-parameter because
  the return value is the `Status`. Ownership passes to the caller.
- `LOCK_EX | LOCK_NB` — exclusive, and **non-blocking**. `LOCK_NB` is the important half:
  without it, opening a database another process holds would hang forever instead of
  reporting an error.
- `EWOULDBLOCK` means the lock is held. It is translated into a message a human can act on.
- `::close(fd)` on the failure path — without it, every failed attempt would leak a
  descriptor.
- `std::make_unique<PosixFileLock>(fd, path)` — the lock is released by
  `PosixFileLock`'s **destructor** (RAII), so an early return anywhere upstream cannot leak
  it. Python's closest equivalent is a `with` block, except this needs no special syntax.

An `flock` lock belongs to the *open file description*, not to the process, which is why
`EnvTest.LockFileIsExclusiveAndReleasedOnDestruction` can prove exclusion from inside a
single test process: a second `open` plus `flock` conflicts with the first.

## New C++ features

Appended to [CPP_NOTES.md](../CPP_NOTES.md): `#pragma once`, `[[nodiscard]]`, `constexpr`
functions and compile-time data, `std::array`, `override` and `final`, `= delete` on copy
operations, function-local `static`, anonymous namespaces, `std::unique_ptr`
out-parameters, `reinterpret_cast`, `struct ::stat info {}`, and the trailing-underscore
member convention.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): advisory lock, compilation database, file
descriptor, FetchContent, fsyncgate, known-answer test, out-of-source build, page cache,
position-independent code, scikit-build-core, short write, translation unit.

## Struggles

No engine-logic bug in this phase took more than one attempt — P0 is scaffolding, and the
logic is small. Two things did go wrong, both worth remembering:

**`pip install .` failed before compiling a single line of C++.** The error arrived as a
`ConfigurationError: Readme file not found ('README.md')` nested inside an
`ExceptionGroup`, roughly thirty lines of traceback for what amounted to "you have not
written the README yet." `pyproject.toml` declared `readme = "README.md"`, and
scikit-build-core validates project metadata *before* it invokes CMake. The lesson that
generalizes: packaging metadata is checked up front, so a dangling file reference in
`pyproject.toml` breaks the install even when the code is perfect. Read the innermost
exception in an `ExceptionGroup` first.

**A one-character typo in the CI workflow.** `steps` was written without its colon. Rather
than discovering it from a red run on GitHub, the workflow was parsed locally with
`yaml.safe_load` first, which pointed at the exact line. Worth keeping as a habit: the
round trip through GitHub Actions is minutes long and its error messages are worse than a
local parser's.

**The first CI run failed, and the reason is worth internalizing.** Not the compiler
warnings that seemed most likely — all twelve build and test jobs passed on both Ubuntu and
macOS — but `clang-format`. Ubuntu's apt ships version 18 and the laptop had Apple's
version 21, and they disagree about whether `struct ::stat info {}` keeps its space before
the braces. There is no way to write that line that satisfies both. The fix was to stop
letting the operating system choose the tool version: all three linters are now pinned in
[requirements-lint.txt](../../requirements-lint.txt) and installed from pip. An unpinned
formatter is not a check, it is a coin flip — and the same reasoning applies to anything
whose output is compared for equality across machines.

**clang-tidy's first run reported 45 diagnostics, and most of them were lies.** Two were
hard errors saying `'string' file not found`, because pip's clang-tidy does not know the
macOS SDK path. That failed parse cascaded: having never seen the declarations in
`crc32c.h`, clang-tidy then "found" that `crc32c::Value` should have internal linkage —
which would have broken the tests that call it. Supplying `-isysroot` removed the errors
*and* those false findings. The real ones were two, and both got fixed: a `const` local
that prevented a move on return, and a C-style array replaced with `std::array`. The lesson
generalizes past this tool: when a static analyzer emits hard errors, fix those first and
do not act on anything else it said.

**One deliberate verification choice.** Rather than trusting remembered CRC32C constants,
a bit-by-bit reference implementation was written in Python
([tools/crc32c_reference.py](../../tools/crc32c_reference.py)) — table-free, so it shares
no code or logic with the C++ version — and used to generate the expected values. All nine
agreed with the C++ table implementation, including the `0xE3069283` the build spec names.
Two independent implementations agreeing is meaningfully stronger evidence than one
implementation agreeing with itself.

## Checkpoint gate

**Do not start P1** until the P0 quiz in the build spec's "Checkpoint quizzes" tab is
answered at 80% or better without notes. Its five questions cover: headers versus source
files, what each of the four presets catches, what undefined behavior is and why sanitizers
matter, what scikit-build-core does during `pip install .`, and what CRC32C is and is not.

Five further questions, written against the real code rather than the concepts, are at the
end of the [P0 debrief](../DEBRIEF.md).
