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

Everything is verified on both operating systems. The first CI run,
[38785287020](https://github.com/marlmelara/strataKV/actions/runs/38785287020), passed all
twelve build, test, and packaging jobs on Ubuntu and macOS, which answered the open
question about `-Werror` with `-Wconversion` and `-Wsign-conversion`: GCC flagged nothing
that Apple Clang had not. Only the lint job failed, for the version-skew reason above.

After pinning the linters, run
[38786203847](https://github.com/marlmelara/strataKV/actions/runs/38786203847) passed all
thirteen jobs. **P0's three acceptance criteria are therefore all met.** The one remaining
annotation was a Node.js 20 deprecation warning on `actions/checkout@v4` and
`actions/setup-python@v5`, since fixed by moving both to `@v7`. Final green run:
[38786517662](https://github.com/marlmelara/strataKV/actions/runs/38786517662).

### Known upcoming risk

GitHub warns that **the `ubuntu-latest` label migrates to Ubuntu 26 beginning October 19,
2026** — four days after this phase was finished. That changes the default GCC, and the
presets compile with `-Werror` alongside `-Wconversion` and `-Wsign-conversion`, so a
newer GCC finding something the current one did not would turn CI red without any code
changing.

This is left deliberately unpinned. Catching a new compiler's warnings is the point of
having those flags, and a storage engine should want to know. But if CI goes red shortly
after October 19 with no commit to blame, this is the first thing to check: the fix is
either to address the warning or to pin `ubuntu-24.04` in the matrix.

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

**Suspended by the owner on 2026-10-15.** The spec gates each phase behind passing that
phase's quiz at 80%; because of a time crunch, the build proceeds without stopping to
self-test. The study material is still written in full every phase, and
[STUDY.md](STUDY.md) is the index for working through it once the build is done.

The trade-off, stated plainly so it is not forgotten: the quizzes existed to catch the gap
between "the code works" and "I can explain the code." Skipping them does not close that
gap, it defers it — and the resume bar in the spec is still twenty questions answered cold.

---

## P1 — WAL, memtable, and crash recovery

### What was built

The write path became real. A write is encoded into a `WriteBatch`, appended to a
write-ahead log as one checksummed record, and `fsync`ed before it is inserted into memory
or acknowledged to the caller. In memory it lands in a skip-list memtable that one writer
can modify while any number of readers search it without taking a lock. Reopening the
database replays the log to rebuild the memtable exactly, discarding a torn final record
but refusing to open if damage is found in the middle of the file. Deletes append
tombstones rather than removing anything, and a crash-point framework can kill the process
at named moments in that sequence on demand.

### The files that matter

| File | What it does |
| --- | --- |
| [src/wal_reader.cc](../src/wal_reader.cc) | Recovery. Scans the whole log before applying any of it, so a failed checksum can be classified as a torn tail or as mid-file corruption by looking at what follows it |
| [src/db_impl.cc](../src/db_impl.cc) | Orchestration, and the one place the durability ordering lives: WAL append, sync, *then* memtable insert, *then* acknowledge |
| [src/skiplist.h](../src/skiplist.h) | The lock-free read path. A node is fully built, then published with a single release store that pairs with readers' acquire loads |
| [src/internal_key.cc](../src/internal_key.cc) | The ordering rule everything depends on: user key ascending, trailer descending, so the newest version of a key is found first |
| [src/wal_writer.cc](../src/wal_writer.cc) | Record framing and the checksum that tells half-written from complete |

### Decisions the spec did not dictate

| Decision | Chose | Alternative considered | Why the alternative lost |
| --- | --- | --- | --- |
| Classifying a failed checksum | Scan the entire log first, then apply only the prefix known to be good | Truncate at the first bad record in one pass, as the teaching snippet does | A single pass cannot tell a torn tail from mid-file damage, because the answer depends on what comes *after* the bad record. Applying as it went would also replay records sitting beyond the damage. The spec's own note on snippet 1 asks for this distinction |
| Reopening the log | Append to the existing log | Start a fresh log and delete the old one, as LevelDB does on open | In P1 there are no SSTables, so the log is the *only* durable copy. Replacing it would discard the entire database on every open |
| Node storage | `string_view` into arena-copied bytes | `std::string` members in the node | The arena frees blocks without running destructors, so a `std::string` member would leak its heap buffer on every node. ASan would have reported it, but the design is simply wrong |
| Variable-height pointers | A separate arena-allocated array, reached through a `next` pointer | The classic trailing `next[1]` "struct hack" that LevelDB uses | Indexing past the end of a one-element array is undefined behavior, and the project runs UBSan with `-fno-sanitize-recover=all`. The pointer version costs one extra arena allocation and is actually defined |
| Arena blocks | `std::vector<std::vector<char>>` | `std::vector<std::unique_ptr<char[]>>` | Same cost, but no C array type for clang-tidy to object to. A moved `std::vector` keeps its heap buffer, so the bump cursor stays valid when the outer vector grows |
| `MemTable::Get` signature | Returns `bool` for "did this table decide?", with the answer in a `Status*` | Return a `Status` and use `NotFound` for both cases | Conflating "not in this table" with "deleted" is exactly how a tombstone stops working: from P2 the caller must keep searching in the first case and stop in the second |
| `WriteOptions::sync` | Can only relax durability, never strengthen it past `Options::sync_policy` | Let a single write force a sync under a `kNoSync` database | A database opened with `kNoSync` has made a weaker promise; letting one call silently exceed it would make the policy meaningless. Combined with `&&`, so the default behaves exactly as the policy says |
| Crash-point testability | A test-only in-process override alongside the environment variable | Environment variable only | `std::_Exit` kills the test runner, so the matching logic could not otherwise be unit tested. The production path is unchanged; the real exit is still verified, via a death test |
| Env additions | Added `NewAppendableFile`, `NewRandomAccessFile`, `TruncateFile` | Read the whole log into memory and rewrite it to truncate | Positional reads are needed by SSTables in P2 anyway, and `truncate()` shortens a file in place without rewriting it. Reading the whole log into memory would also not scale past one memtable |
| Write serialization | One `std::mutex` around the whole write | Start on group commit now | Group commit is P4. One writer at a time is also precisely the skip list's contract, so this is the correct intermediate step rather than a shortcut |
| Log file numbering | Fixed at `000001.log` | Invent a numbering scheme now | File numbers come from the MANIFEST, which is P3. A zero-padded helper is in place so the naming does not change later |

### Bugs that took more than one attempt

None. The code compiled and all 111 tests passed on the first run, which is worth stating
plainly rather than dressing up. Three things are still worth recording:

1. **A C++ scoping trap caught before it compiled.** `SkipList::Iterator` holds a `Node*`,
   but `Node` is a private member of the enclosing class declared *after* `Iterator`.
   Writing `struct Node* node_` there does not fail loudly — the elaborated type specifier
   quietly declares a *new* incomplete type, which may or may not end up being the same
   `SkipList::Node`. The fix is a forward declaration of `Node` before `Iterator`. Worth
   knowing because the failure mode is confusing errors far from the cause, not a clear
   message at the mistake. (Member function *bodies* written inside a class are fine, since
   those are compiled as though the class were already complete.)

2. **Switching C arrays to `std::array` produced three new compile errors.** `std::array`
   indexes with `size_t` while the skip list's level counters are `int`, and the build
   treats an implicit signed-to-unsigned conversion as an error. That is the warning doing
   its job: the casts are now explicit and visible. A reminder that "modernize" changes are
   not free refactors.

3. **clang-tidy produced 13 findings, 11 of which were worth taking.** Two checks gave
   advice that could not be followed: `misc-const-correctness` wanted the pointee of
   `Arena::Allocate`'s return value made `const`, which would not compile given the
   function's entire purpose, and `bugprone-empty-catch` objected to the destructor
   swallowing exceptions, which is mandatory rather than sloppy. Both are now excluded with
   the reason recorded in [.clang-tidy](../.clang-tidy). The other eleven were real:
   oversized enums, C arrays, and a destructor that could let an exception escape.

### The one genuinely hard design question

Not the skip list — the teaching snippet is accurate and translated almost directly. It was
**what a failed checksum means.**

A record whose bytes are all present but whose checksum fails is ambiguous. It could be the
last thing an interrupted write was flushing, or it could be damage in the middle of a log
that was written completely. Truncating in the first case is correct and loses nothing;
truncating in the second throws away good data that follows. Nothing about the record
itself distinguishes them — only what comes *after* it does. That is why recovery scans the
entire file before applying any of it, and why the acceptance tests for the two cases are
genuinely testing different code paths rather than the same one twice.

### Found by the post-phase verification pass

Re-checking the phase before moving on turned up two things that the original test suite
did not cover.

**A failed open releases the LOCK file — verified, not assumed.** Reasoning said the
destructor would run `Close()` and reset the lock, but nothing tested it. If it were wrong,
a corrupt database could never be retried or repaired without restarting the process.
Two tests now cover it, including three consecutive failed opens.

**A damaged *final* WAL record is silently truncated rather than reported.** This came out
of writing the test above with a single record, which unexpectedly opened cleanly. The
cause is not a bug: with one record there is nothing valid after the damage, so the
classification rule treats it as a torn tail. But it means that if disk corruption damages
the last acknowledged write, the engine drops it and reports a successful open.

Nothing in the log alone can distinguish the two cases — the bytes are identical, and the
log never records how many records it should contain. The MANIFEST in P3 fixes this by
recording the last flushed sequence number, which lets recovery notice that the log ends
earlier than the recorded state implies. The behavior is now pinned by
`DBTest.ADamagedFinalRecordIsTruncatedNotReported` and written up under "Known limits of
recovery" in [DESIGN.md](DESIGN.md), so P3 changes it on purpose rather than by accident.

**Leak detection does not run locally.** Forcing `ASAN_OPTIONS=detect_leaks=1` on
macOS/arm64 produces `detect_leaks is not supported on this platform` and aborts every
test. LeakSanitizer is enabled by default with ASan on Linux, so the Ubuntu `asan` CI job
is the only place leaks are actually checked — which matters for this phase in particular,
since the arena hands out memory that is never individually freed. That job passes. Worth
knowing so a clean local ASan run is not mistaken for a leak check.

### Five questions

1. `Write` appends to the WAL, syncs, and only then inserts into the memtable. What happens
   if the process dies between the sync and the insert — and what would go wrong if those
   two steps were swapped?
2. A WAL record's bytes are all present but its checksum fails. Why can the recovery code
   not decide what to do about it yet, and what piece of information settles it?
3. `MemTable::Get` returns `false` for a key it has never seen, but `true` with a `NotFound`
   status for a key that was deleted. Why does collapsing those two into one answer break
   the engine?
4. The skip list publishes a new node with `memory_order_release` and readers follow
   pointers with `memory_order_acquire`. What exactly could a reader observe if the
   publishing store were `memory_order_relaxed`, and why might that bug never appear on an
   x86 laptop?
5. Skip-list nodes hold `std::string_view`s into arena memory instead of `std::string`s.
   What specifically goes wrong if that is changed to `std::string`?
6. Disk corruption damages the *last* record in the WAL of a database with one acknowledged
   write. What does the next open do, why can P1 not do better, and what piece of
   information would make it possible?

#### Answers

1. Nothing is lost. The record is already durable, so replay rebuilds the memtable from the
   log on the next open. Swapped, a crash between them would leave the write visible in
   memory and acknowledged to the caller, but absent from disk — the next open would
   silently lose an acknowledged write, which is the one thing the project promises cannot
   happen.
2. Because the two possibilities — an interrupted write at the tail, and damage in a file
   that was fully written — look identical at that record. What settles it is whether any
   *later* record in the file has a valid checksum. If one does, the file was complete and
   something corrupted it, so opening must fail. If nothing valid follows, it was the tail
   and truncating it is correct.
3. `false` means "I do not know, keep searching older data"; `true` means "I have the
   answer." A tombstone *is* an answer. If a delete returned `false`, the caller would move
   on to the next level down, find the older value that the tombstone exists to hide, and
   return a deleted key as though it were live.
4. A reader could see the pointer to the new node before seeing the node's key, value, and
   forward pointers, and read uninitialized memory. The release/acquire pair forbids that
   reordering. x86 has a strongly ordered memory model that happens to prevent most such
   reorderings in practice, so the bug can hide there and appear on ARM — Apple Silicon
   included — which reorders much more aggressively.
5. The arena frees its blocks as raw memory without running destructors, so every node's
   `std::string` would leak its heap buffer. The `string_view` points at bytes the arena
   itself copied and owns, so freeing the blocks frees everything exactly once. This is
   also why nodes are safe to reach through raw pointers with no lock: they are never freed
   individually.
6. It truncates the damaged record and opens successfully with an empty database, silently
   losing an acknowledged write. P1 cannot do better because a damaged last record is
   byte-for-byte indistinguishable from a write that was interrupted mid-append, and the
   log never records how many records it should contain — so there is nothing to compare
   against. The missing information is the last sequence number the database is known to
   have reached, which the MANIFEST records from P3 onward.

### Numbers

P1 produced no performance numbers; the first ones arrive in P4. Test counts and sanitizer
results are in [NUMBERS.md](NUMBERS.md).

### Gate

Suspended; see [STUDY.md](STUDY.md). P2 follows without waiting on a quiz pass.

---

## P2 — SSTables, flush, and the read path

### What was built

Data now reaches disk. When the memtable passes its size limit it is set aside and a
background thread writes it out as an SSTable: sorted data blocks, a bloom filter over the
user keys, an index of each block's last key, and a fixed-size footer with a magic number
and checksum. Reads walk three layers newest-first -- active memtable, immutable memtable,
then L0 tables -- stopping at the first answer, with the bloom filter sparing most lookups
any disk read at all. A log is deleted only once the table built from it is durable, so a
crash anywhere in the flush leaves the data recoverable from one place or the other.

### The files that matter

| File | What it does |
| --- | --- |
| [src/sstable_reader.cc](../src/sstable_reader.cc) | The read side of a table: footer validation, bloom check, index binary search, one block read. Also where every offset read from the file is bounds-checked, because they are untrusted input |
| [src/db_impl.cc](../src/db_impl.cc) | Rotation, the background flush loop, the three-layer read path, and the ordering that makes a flush crash-safe |
| [src/sstable_builder.cc](../src/sstable_builder.cc) | Writes the four sections and refuses out-of-order keys, which the index search and block scan both depend on |
| [src/bloom.cc](../src/bloom.cc) | Double hashing, probe-count arithmetic, and a filter that degrades to "maybe" rather than lying when a block is malformed |
| [src/filename.h](../src/filename.h) | Without a MANIFEST, the directory listing *is* the record of what exists; this parses it |

### Decisions the spec did not dictate

| Decision | Chose | Alternative considered | Why the alternative lost |
| --- | --- | --- | --- |
| Recovering sequence numbers after every log is flushed | Write a zero-operation "sequence watermark" record into each newly created log | Read every SSTable at open to find the highest sequence number | The alternative is O(data) work on every open. Without either, numbering restarts at zero and new writes sort as *older* than data already on disk, so reads silently return stale values. See the struggles section -- this was the one real hole in the phase |
| What records that a table exists | The table file itself, synced, with its directory synced | A small durable file listing the tables | That is the MANIFEST, which is P3. Scanning the directory is enough for P2 and is thrown away rather than built on |
| An SSTable whose footer does not validate | Delete it during recovery | Keep it and report Corruption | A half-written table is exactly what a crash during a flush leaves, and its data is guaranteed to still be in a log, because a log is only deleted after its table is durable. Refusing to open would make a normal crash unrecoverable |
| Logs per memtable | A list, not a single number | One log number per memtable | Recovery may replay several logs into one memtable, and all of them must survive until that memtable is flushed |
| L0 ordering | Newest file first, by file number | Sort by key range | L0 files overlap by nature, since each is whatever keys happened to be written. Only recency can order them |
| Holding files open for readers | A `shared_ptr` to an immutable vector of readers, copied under the lock | Re-open files per read, or lock across reads | A reader keeps the files it started with alive even as the flush installs a new set. This is P3's Version idea in miniature, and it is what lets a read hold no lock during disk I/O |
| Flush failure | Record it and stop accepting writes | Retry, or ignore it | A memtable that cannot be flushed will never free its log. Continuing would accumulate unbounded state while pretending to work |
| On shutdown | Leave a pending memtable unflushed | Flush before closing | Its log is still on disk, so recovery restores it. Flushing would make `Close` take arbitrarily long for no gain in durability |
| The 1,000,000-key acceptance test | Default 4 MB memtable, batched writes, `kNoSync` | One synced `Put` per key | `kSyncEveryWrite` with `F_FULLFSYNC` would mean a million physical flushes and hours of runtime. Durability has its own dedicated tests; this one is about volume. It runs in 12 seconds |
| Crash-point testing | A separate helper binary run as a child process | In-process death tests | A crash point calls `_Exit`, which would take the test runner with it. The helper prints each key only *after* its `Put` returns, so its output is exactly the set of acknowledged writes -- the same design P5's fuzzer will use |

### An objection, as the spec asks for one

The spec fixes the data block encoding as "a plain `klen | key | vlen | value` encoding" with
no per-block checksum; only the footer has one. The implementation follows that exactly.

**The objection:** this means corruption inside a data block is undetectable. The footer
checksum proves the table was written completely, not that its contents are still intact.
A flipped bit in a data block will be returned to the caller as a value, silently. Compare
the WAL, where every record is checksummed and damage is caught.

`ReadOptions::verify_checksums` therefore has nothing to verify at block level, which is
the clearest sign something is missing.

**What I would change:** add a 4-byte CRC32C to the end of each data block and check it on
read. The cost is 4 bytes per ~4 KB (0.1%) and one checksum over data already in cache.
LevelDB and RocksDB both do this. I have not changed it, because the format is specified
and `docs/DESIGN.md` records the trade-off -- but it is the first thing I would raise before
P6, since "crash-safe" is the project's headline claim and this is the one place where
damage goes unreported.

### Bugs that took more than one attempt

**The sequence watermark, which was a genuine design hole rather than a slip.** The first
implementation flushed memtables and deleted their logs, exactly as the spec describes. It
passed every test I had written. Then I reasoned through what happens when *all* logs have
been flushed and deleted: the next open replays a fresh, empty log, learns nothing about
sequence numbers, and restarts from zero. New writes then get numbers lower than data
already in the SSTables, so they sort as older, and `Get` returns the stale value -- a
silent wrong-answer bug, the worst kind for a storage engine.

The fix is one record per log. What is worth keeping is how it was found: not by a failing
test, but by asking "what is the state of the system after the last log is deleted?" The
test (`SequenceNumbersContinueAcrossAFullFlushAndReopen`) was written afterwards, to pin it.

**Two test failures that were both my fault, not the engine's.** Worth recording because
the instinct on a red test is to suspect the code:

1. `SSTableTest.HandlesEmptyAndBinaryKeysAndValues` segfaulted. The cause was my test data:
   I listed `""`, `"k\0ey"`, `"empty-value"` as sorted, but `'e' < 'k'`, so `"empty-value"`
   must come second. The builder correctly rejected the out-of-order key; my helper then
   dereferenced the null reader from the table it had failed to finish. Two lessons: the
   ordering check earned its place immediately, and a test helper should abort on a null
   rather than hand it back to segfault at first use.
2. Two crash-point cases failed on `ASSERT_GT(acknowledged.size(), 0)`. Both
   `write_before_wal` and `write_after_wal_sync` fire on the very *first* write, so nothing
   is ever acknowledged and an empty list is the correct result. They are now a separate
   suite asking the right question -- does the database still open and work? -- while the
   flush crash points keep the "nothing acknowledged was lost" assertion, where thousands
   of writes make it meaningful.

### Five questions

1. The flush writes an SSTable, syncs it, syncs the directory, installs it, and only then
   deletes the log. Why does the directory have to be synced separately from the file, and
   what breaks if the log is deleted one step earlier?
2. Recovery finds an SSTable whose footer does not validate. Why is deleting it safe rather
   than reckless?
3. Every log has been flushed and deleted. What goes wrong on the next open without the
   sequence watermark record, and why is the symptom worse than a crash would be?
4. A `Get` for a key that is in no table at all. How many disk reads does it cost with ten
   L0 files, and how many without bloom filters?
5. `MakeRoomForWrite` waits when `imm_` is already set instead of rotating again. What does
   that waiting actually protect?

#### Answers

1. A file's contents and its *name* are separate pieces of data. Syncing the file makes the
   bytes durable; the directory entry that gives it a name can still be lost, so the table
   could vanish after a crash. Deleting the log earlier would leave a window where neither
   the log nor a durable table holds the data, losing everything the memtable contained.
2. Because a log is only ever deleted *after* its table is durable, so an invalid table's
   data is guaranteed to still be in a log that was not deleted. The invalid file is exactly
   what a crash mid-flush leaves behind. Refusing to open instead would turn an ordinary
   crash into an unrecoverable database.
3. Recovery replays an empty log, learns nothing, and restarts sequence numbering from zero.
   New writes then get sequence numbers lower than entries already in the SSTables, so the
   ordering rule treats them as older and `Get` returns stale values. It is worse than a
   crash because nothing fails: the database reports success and serves wrong data.
4. Usually zero. Each table's bloom filter is consulted in memory, and at ~1% false
   positives, ten filters give about a 10% chance of a single wasted 4 KB read. Without
   filters it would be ten reads -- one block from every table -- for a key that exists
   nowhere.
5. Memory. Rotating again would hand over a second memtable while the first is still being
   written, and nothing would stop a third, so a writer faster than the disk could pile up
   unboundedly many. Waiting keeps at most two memtables alive at once.

### Numbers

Recorded in [NUMBERS.md](NUMBERS.md): the measured bloom filter false-positive rate, the
1,000,000-key round trip, and the sanitizer results. No throughput numbers yet -- those
start in P4.

### Gate

Suspended; see [STUDY.md](STUDY.md).
