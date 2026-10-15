# Debrief

One section per phase: what was built, which files matter, what was decided that the spec
did not dictate, what broke, and five questions to test yourself with.

---

## P0 — Scaffold and CI

### What was built

A CMake project that builds the engine four different ways — plain debug, optimized,
AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer — with GoogleTest pulled
in automatically. The first real engine code landed: `Status` for error returns, `Options`
for configuration, `Env` as the single place every syscall goes through, and a hand-written
CRC32C. A pybind11 module and `pyproject.toml` make `pip install .` compile the C++ and
produce an importable `stratakv` package. A GitHub Actions workflow runs all four presets
plus the Python tests on both Ubuntu and macOS, with a separate formatting and lint job.
Twenty-six C++ tests and five Python tests pass, and the sanitizer presets report nothing.

### The files that matter

| File | What it does |
| --- | --- |
| [src/env.cc](../src/env.cc) | The whole OS layer. Every `fsync`, rename, directory sync, and `flock` the engine will ever do lives here, which is why the durability rules can be reviewed in one place. Contains the `F_FULLFSYNC` handling and the sticky-sync-failure logic |
| [include/stratakv/status.h](../include/stratakv/status.h) | The error type returned by essentially every engine function. Marked `[[nodiscard]]`, so ignoring a failure is a compile warning rather than a silent bug |
| [src/crc32c.cc](../src/crc32c.cc) | The checksum that tells a torn write apart from a complete one. Compile-time table, no dependency |
| [CMakeLists.txt](../CMakeLists.txt) + [CMakePresets.json](../CMakePresets.json) | Defines the four presets. The sanitizer presets are the actual correctness gate for every later phase |
| [.github/workflows/ci.yml](../.github/workflows/ci.yml) | Runs all four presets on two operating systems, plus the Python install and the lint job |

### Decisions the spec did not dictate

| Decision | Chose | Alternative considered | Why the alternative lost |
| --- | --- | --- | --- |
| `Status` internals | `StatusCode` enum plus an owned `std::string` | LevelDB's packed `const char*` with a manual length prefix | The packed form saves one allocation on the error path, which is not a hot path. The spec says to keep the code readable for a learner, and this is a case where the clever version buys nothing |
| Enforcing checked results | `[[nodiscard]]` on the whole `Status` class | Relying on convention | The stated cost of returning statuses instead of throwing is "every result must be checked." Making the compiler do it costs one attribute |
| Lock release | RAII — destroying the `FileLock` unlocks and closes | LevelDB's explicit `Env::UnlockFile(lock)` | An early return cannot leak the lock, and it gives a concrete example of the RAII idea the engine leans on everywhere. Cost: a destructor cannot report an unlock failure, which is not actionable anyway |
| Failed `fsync` | Sticky. The error is remembered and returned from every later `Append`, `Sync`, and `Truncate` | Return the error once and let the caller decide | A second `fsync` can report success after the kernel has discarded the data (fsyncgate). A sticky error makes it impossible to acknowledge a write on top of a sync that failed |
| CRC32C table | Built by a `constexpr` function at compile time | Runtime init on first use, or a hardware `crc32` instruction | `constexpr` removes static initialization order questions entirely. Hardware CRC32C via SSE4.2 or ARM is faster but platform-specific; correctness first, and P6 can revisit if checksums ever show up in a profile |
| `Env` shape | Abstract base class, one `PosixEnv` behind `Env::Default()` | A single concrete class with no virtuals | Costs a virtual call per syscall, which is nothing next to the syscall itself, and leaves room for a fake Env later without touching call sites |
| `SyncFile` opens for writing | `O_WRONLY` | `O_RDONLY` | POSIX permits `fsync` to fail on a descriptor not open for writing. The only files the engine syncs by path are ones it just wrote, so write access is always available |
| clang-tidy locally | Config committed, enforced in CI only | `brew install llvm` to get it locally | clang-tidy is not in Apple's Command Line Tools, and pulling the full LLVM toolchain is a large dependency for a check CI already runs on every push |
| Python test matrix | 3.11 and 3.13 in CI | Only the local 3.14 | Testing the floor of `requires-python` is the point; 3.14 is what gets exercised locally anyway |

### Bugs that took more than one attempt

Honestly: none in the engine code. P0 is scaffolding, and the two things that broke were
both build-configuration problems, each fixed on the first try once the error was read:

1. **`pip install .` failed before compiling anything.** scikit-build-core validates
   project metadata before it runs CMake, and `pyproject.toml` declared
   `readme = "README.md"` when no README existed yet. The error surfaced as a
   `ConfigurationError: Readme file not found` buried inside an `ExceptionGroup` traceback,
   which looked far more alarming than "you have not written the README yet." Lesson:
   metadata is validated before the build, so a missing file referenced from
   `pyproject.toml` fails the install even though the C++ is fine.

2. **A typo in the CI workflow (`steps` without its colon).** Caught by parsing the YAML
   locally before committing rather than by pushing and waiting for a red run. Lesson worth
   keeping: validate workflow files locally; the feedback loop through GitHub is minutes
   long and the error message there is worse.

3. **The first CI run went red on formatting — a tool version mismatch, not the code.**
   Ubuntu's apt ships `clang-format` 18; the development machine had Apple's
   `clang-format` 21. They disagree about whether `struct ::stat info {}` keeps the space
   before the braces, and *no single spelling satisfies both* — formatting to suit one
   makes the other reject it. Contorting the code would have been the wrong fix. Instead
   all three linters are now pinned in `requirements-lint.txt` and installed from pip in
   CI rather than from apt, so the question "which clang-format?" has exactly one answer.
   Lesson: a formatter that is not version-pinned is not a check, it is a coin flip.

4. **clang-tidy had never actually run, and reported 45 diagnostics on first use.** Two
   were hard errors — `'string' file not found` — which turned out to be environmental:
   pip's clang-tidy is not Apple's toolchain and does not know where the macOS SDK keeps
   the standard library headers. Passing `-isysroot` fixed those and, importantly, also
   removed several *false* findings that the failed header parse had caused (clang-tidy had
   lost the declarations in `crc32c.h` and was recommending internal linkage for functions
   the tests call). The platform difference now lives in `tools/run-clang-tidy.sh` so the
   local and CI invocations are the same one. Of the genuine findings, the check list was
   curated down to the families that catch defects, and the two real ones were fixed: a
   `const` local that blocked a move on return, and a C-style array replaced with
   `std::array`. Lesson: a hard error from a static analyzer poisons its other output —
   fix the errors before believing any of the warnings.

### Verification status

Everything is verified on both operating systems. CI run
[38785287020](https://github.com/marlmelara/strataKV/actions/runs/38785287020) passed all
twelve build, test, and packaging jobs on Ubuntu and macOS, which answered the open
question about `-Werror` with `-Wconversion` and `-Wsign-conversion`: GCC flagged nothing
that Apple Clang had not. Only the lint job failed, for the version-skew reason above.

### Five questions

1. A `Put` has appended its record to the WAL and `Append` returned OK. Has the write been
   acknowledged? What is still missing, and where does the data physically live right now?
2. `Env::Sync` tries `fcntl(fd, F_FULLFSYNC)` first and falls back to `fsync` if it fails.
   Why is the fallback correct rather than a silent downgrade of the durability guarantee?
3. `PosixWritableFile` remembers a failed `fsync` and returns that error from every later
   call. What goes wrong if it instead returns the error once and lets the caller retry?
4. The CRC32C test checks `"123456789"` against `0xE3069283`, a constant from an external
   standard, instead of against whatever the implementation produces. What class of bug does
   that catch that a self-consistency test cannot?
5. `NewWritableFile` returns a `Status` and hands back the file through a
   `std::unique_ptr<WritableFile>*` out-parameter. Who closes the file, and when?

#### Answers

1. No. `Append` only means the bytes reached the kernel's page cache; a crash now loses
   them. The write is acknowledged only after `Sync()` returns OK, and on macOS only
   because `Sync` uses `F_FULLFSYNC` to make the drive flush its own cache.
2. Because `F_FULLFSYNC` is unsupported on some filesystems and network mounts, where it
   fails immediately rather than doing less work. On a filesystem that does support it, it
   is used. Falling back is a downgrade only on filesystems that cannot offer the stronger
   guarantee at all, where the alternative is failing every write.
3. The retry can report success while the data is gone: on Linux the kernel may have already
   dropped the dirty pages and cleared the error flag. The engine would then acknowledge a
   write on top of a sync that never happened. This is "fsyncgate"; Postgres responded by
   crashing on `fsync` failure.
4. An implementation that is wrong but internally consistent — a flipped polynomial, a
   missing final inversion, bytes processed in the wrong order. Checksums would verify fine
   within this build and fail against any other CRC32C implementation, and files written by
   an older build would stop verifying after a "harmless" refactor.
5. The caller. Ownership transfers with the `unique_ptr`, so the file is closed when that
   pointer goes out of scope (RAII) — or earlier and with error reporting, if the caller
   calls `Close()` explicitly.

### Numbers

P0 produced no performance numbers, and none were invented. The pass/fail results and the
machine profile are recorded in [NUMBERS.md](NUMBERS.md).

### Gate

Do not start P1 until the P0 quiz in the build spec's "Checkpoint quizzes" tab is answered
at 80% or better, without notes.
