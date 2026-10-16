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

### Found by the post-phase verification pass

P2 passed everything on the first full run, which is exactly when it is worth looking
harder. Re-checking the phase before starting P3 found four things the test suite did not
cover. None was a bug, but three of them were paths where a bug would have gone unnoticed.

**Concurrent reads during a flush were never exercised.** This is the interaction P2
introduced: a reader copies three pointers under the lock, releases it, and searches while
the background thread rotates memtables, writes a table, and swaps the L0 list underneath
it. Every existing test waited for the flush to finish first.
`ReadersSeeEveryAcknowledgedKeyWhileFlushesRun` now runs four reader threads against a
writer forcing repeated rotations, asserting that a key acknowledged before a read begins
can never vanish during one. It passed 20 consecutive runs under ThreadSanitizer.

**Nothing checked that logs left by a crash are ever cleaned up.** After a crash in the
flush window, recovery replays several logs into one memtable, and all of them must be
deleted when that memtable is next flushed. If the bookkeeping were wrong, logs would
accumulate forever: the database would grow without bound even though its contents did
not, and recovery would get slower on every open. That is the kind of defect that only
shows up in production, weeks later.
`LogsLeftByACrashAreCleanedUpByTheNextFlush` pins it.

**`ParseFileName` had no unit tests at all.** It parses the directory listing, so it is the
code that decides what the engine believes exists, and it carries both a digit check and an
overflow guard that nothing verified. Six tests now cover round-tripping, names that merely
resemble ours (`000001.sstx`, `000001.lo`, `1.5.log`), and a 20-digit number that would
overflow into a small one and collide with a real file.

**A stray file in the database directory was untested.** `StrayFilesInTheDirectoryAreIgnored`
confirms that `notes.txt` and friends are left alone -- recovery has no business deleting
files it does not recognise.

### Still not covered, deliberately

**The flush-failure path.** If a flush fails, `background_error_` is set and writes stop.
There is no clean way to make a flush fail on demand without adding a fault-injection hook
to `Env`, which is worth doing when P3 adds more I/O paths rather than bolting on now. The
path is three lines and reviewed, but it is not tested, and that is worth stating rather
than leaving implied.

**Deleting log files by hand.** Every log begins with a sequence watermark, and the active
log is never deleted by the engine, so sequence numbering always survives a flush. But if
someone removes the logs manually while leaving the SSTables, recovery would restart
numbering from zero and new writes would be shadowed by older data. There is no cheap
defense in P2; the MANIFEST in P3 makes it detectable. Not a supported operation, and now
written down as such.

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

---

## P3 — MANIFEST, versions, and compaction

### What was built

Compaction, and the bookkeeping that makes it safe. Once four L0 files exist they are merged
with the overlapping L1 files and rewritten as fresh non-overlapping L1 files, dropping
versions a newer write has replaced and tombstones that have nothing left to hide. A MANIFEST
records every change to the file set as an appended edit, and CURRENT names the live MANIFEST,
replaced by the four-step write-sync-rename-sync dance. Reads hold a reference-counted
immutable Version, so files cannot be deleted underneath them, and in-flight read snapshots
are registered so compaction never drops a version a live read still needs.

### The files that matter

| File | What it does |
| --- | --- |
| [src/compaction.cc](../src/compaction.cc) | The merge and the drop rule -- the hardest logic in the project, and the only place that decides what is allowed to stop existing |
| [src/version.cc](../src/version.cc) | VersionEdit encoding, the MANIFEST, the four-step CURRENT replacement, and the reference counting that keeps a reader's files alive |
| [src/db_impl.cc](../src/db_impl.cc) | Rewired onto the version set; adds the read-snapshot registry and obsolete-file deletion |
| [src/merging_iterator.cc](../src/merging_iterator.cc) | k-way merge with a min-heap, so all versions of one key arrive together, newest first |
| [src/iterator.h](../src/iterator.h) | One cursor interface, so the merge does not care whether an input is a table on disk or a memtable |

### Decisions the spec did not dictate

| Decision | Chose | Alternative considered | Why the alternative lost |
| --- | --- | --- | --- |
| Protecting in-flight reads from compaction | Register every read's snapshot in a multiset; compaction never drops a version at or above the oldest one | Drop every shadowed version, as the teaching snippet does | The snippet is correct for a database with no concurrent reads. Here, a read captures its snapshot and then searches without a lock, so a write plus a completed compaction can happen underneath it. Dropping the version it needed makes an existing key read as **NotFound** -- a silent wrong answer. Costs one multiset insert and erase per read |
| Where open readers live | Inside the Version, alongside the file metadata | A separate table cache keyed by file number, as LevelDB has | Keeps everything in `version.{h,cc}` as the spec's layout specifies, and makes "a reader holds a Version so its files cannot be deleted" literally true rather than a convention. A cache would be better once there are many levels |
| Splitting compaction output | Only at a user-key boundary | Split whenever the target size is reached | Two L1 files both holding one user key would overlap, and the read path binary searches L1 assuming no overlap. The bug would be silently missing data, not a crash |
| MANIFEST framing | Reuse the WAL's record framing, factored into `ReplayRecords` | A second record reader for the MANIFEST | The spec says "the same record framing as the WAL". Reusing it also reuses the torn-tail and mid-file-corruption rules, which took real thought to get right in P1 and would have drifted if reimplemented |
| MANIFEST lifetime | Start a fresh MANIFEST on every open, beginning with a snapshot of the whole state | Append to the existing one forever | A MANIFEST appended to across thousands of restarts grows without bound and makes recovery slower every time. A fresh one costs one snapshot record |
| Sequence watermark | Kept from P2 | Removed, now that the MANIFEST records the last sequence | The MANIFEST's last sequence is only as current as the newest edit; a log may hold acknowledged writes the MANIFEST has not seen. Both are needed, and they take the maximum |
| Obsolete file deletion | Scan the directory and delete anything no live version references | Track deletions explicitly in the edit | The scan is simple and self-correcting: it also cleans up outputs of a compaction that crashed before its MANIFEST record landed, which an explicit list would miss |
| Compaction scheduling | The same background thread as the flush, flush first | A second thread | One thread means flush and compaction can never race each other for the same files, and the ordering is obvious. A second thread is a P4 concern if throughput demands it |
| The 5,000,000-operation test under sanitizers | Skipped, with an explanatory message | Run it everywhere | Instrumented it would add roughly half an hour to every sweep, and it is a volume test. The tests that are actually about races and memory errors run under both sanitizers at a size that finishes |

### Bugs that took more than one attempt

Three, and each was a genuine defect rather than a slip.

**1. `RemoveObsoleteFiles` deleted the MANIFEST it had just written.** The worst of the
three. The cutoff for which MANIFESTs to keep was compared against `next_file_number_`
instead of the number of the live MANIFEST, so the file CURRENT pointed at was always
below the cutoff and always deleted. The symptom was spectacular and immediate -- every
reopen failed with "MANIFEST contains no usable edits", taking twelve tests down with it --
which is the good case. Had the comparison been wrong in the other direction, old MANIFESTs
would have accumulated silently forever.

**2. The compaction held a reference to the version it was obsoleting.** `RunCompaction`
copied `current_` into a local `shared_ptr`, did its work, and then asked for obsolete files
to be deleted -- while still holding that local reference. Since holding a Version is
*precisely* the mechanism that keeps its files alive, the compaction was sparing exactly the
files it had just made redundant. Twenty-two tables sat on disk where the version referenced
far fewer.

This one is worth dwelling on because nothing was broken: the reference counting did exactly
what it is designed to do. The caller was wrong, not the mechanism. The fix is one line --
`version.reset()` before the cleanup -- and a comment explaining why it cannot be left to
the end of the function. `ObsoleteFilesAreDeletedAfterCompaction` is the test that caught it.

**3. The space-reclamation test measured nothing.** It wrote forty generations of the same
200 keys, waited for background work, measured, compacted, and measured again -- but waiting
for background work *is* waiting for compaction, so the "before" measurement was already
compacted and the second compaction found an empty L0 and did nothing. The test asserted
`after < before` and failed, correctly, by being meaningless. Fixed by holding automatic
compaction off with a high trigger so the measurement has something to measure, and by
asserting there is more than one L0 file first, so the test cannot pass vacuously again.

**Also worth recording: thirteen test failures that were adaptation, not breakage.** Adding
the MANIFEST shifted every file number by one, so tests hardcoding `000001.log` broke; and
compaction now drains L0, so tests asserting `L0 > 1` broke. Both were real behavior changes
and the tests were updated to match -- the log path is now discovered rather than hardcoded,
and the level assertions ask about total files rather than L0 specifically.

### Measured numbers

The acceptance run: 5,000,000 operations over a 400,000-key space with 10% deletes, compared
against a reference `std::map` of 360,035 surviving keys -- every one present with the right
value, every deleted key absent.

**Space amplification came out at 1.44x**, inside the required 1.5x but not by much. That
margin is worth understanding rather than celebrating: the denominator is the raw key and
value bytes, while the files also carry an 8-byte internal-key trailer per entry (about
2.9 MB across 360,035 keys), plus a bloom filter, an index, and a footer per file, plus any
L0 files not yet compacted. Most of the 1.44x is unavoidable format overhead rather than
garbage, which also means the figure would not improve much with more aggressive compaction.

### Found by the post-phase verification pass

Four gaps, and one genuinely surprising result.

**The MANIFEST had never been damaged in a test.** It is now the authoritative record of
what the database consists of -- more load-bearing than any single log -- and nothing
checked what happens when it is truncated or empty. Three tests now cover it: a torn tail is
dropped and recovery lands on the last complete edit (verified by asserting the recovered
log number is the fourth edit's, not the fifth's), an empty MANIFEST is reported as
corruption, and a MANIFEST that CURRENT names but which is missing does not open as a
silently empty database.

**Orphaned compaction outputs were not checked.** A compaction that crashes before its
MANIFEST record lands leaves tables nothing references. If they were not cleaned up, every
crash during a compaction would leak disk space permanently.
`OrphanedCompactionOutputsAreDeletedOnOpen` pins it.

**A file the MANIFEST lists but which is missing** is a real inconsistency now that the
MANIFEST is authoritative -- in P2 the directory scan simply would not have seen it. It must
be reported, not opened with a hole in it.

**The surprising one: my end-to-end test for the read-snapshot registry does not actually
test it.** I wrote a test with four readers, a writer churning versions, and repeated
compactions, on the theory that a broken registry would show up as a key reading as absent.
Then I disabled the registry to check the test detected its absence.

**It passed anyway.**

The window is simply too narrow to hit by chance. For a reader to be harmed, a write has to
land after it captures its snapshot *and* a whole compaction has to finish before the read
does -- and a read completes in microseconds while a compaction takes milliseconds. Running
the system and hoping to race into it is not a strategy.

I had written "without the registry this fails; verified by removing it" in the test comment
*before* checking. That was an unverified claim, and it was wrong. The comment now says what
is actually true: the test is a useful smoke test for the concurrent paths, and the
deterministic unit tests -- `KeepsAShadowedVersionThatALiveSnapshotStillNeeds` and
`KeepsATombstoneALiveSnapshotStillNeeds` -- are what justify the registry, because they
assert the rule directly instead of hoping to observe its absence.

The lesson generalizes well beyond this project: **a correctness rule that only matters in a
rare interleaving must be tested by asserting the rule, not by exercising the system.** The
integration test tells you the parts fit together; only the unit test tells you the rule is
right.

### Still not covered, deliberately

**The flush- and compaction-failure branches.** Both set `background_error_` and stop
accepting writes. Triggering them needs a fault-injection hook in `Env`, which is worth
adding when P4 touches these paths again rather than bolting on now. Reviewed, not tested,
and said so rather than implied.

**A reader holding a Version for a very long time.** Files it references cannot be deleted,
so a pathologically slow reader delays reclamation. That is the intended trade-off, but
nothing bounds it, and nothing tests it.

### A bug CI caught that the whole C++ suite missed

P3 added `src/version.cc` for the `VersionSet`. P0 already had a file of that name, holding
the one-line function that returns the engine's version string. **The new file silently
overwrote it**, so `stratakv::Version()` no longer had a definition anywhere.

Every one of the 212 C++ tests still passed, because nothing in them called that function:
it existed only for the Python binding to expose. A static library links fine with an
undefined symbol nobody references. The failure surfaced as `symbol not found in flat
namespace` when the extension module was loaded -- on both operating systems, in CI, after
the commit was already pushed.

Two things were wrong, and both are fixed:

1. **A name collision.** `stratakv::Version` was now both a class (the immutable file-set
   snapshot) and a function (the version string). That compiled only because no translation
   unit happened to include both `include/stratakv/version.h` and `src/version.h` -- luck,
   not design, and it would have broken the moment some header pulled in both. The function
   is now `VersionString()` in `src/version_string.cc`, which cannot collide.

2. **A coverage hole that made the first problem invisible.** The version string was reachable
   only through Python, so a C++-only change could break it while the C++ suite stayed green.
   `test_version_string.cc` now references it from four C++ tests, so a missing definition
   fails the C++ build immediately.

The lesson worth keeping: **a symbol only exercised by one binding is effectively untested by
the other.** The C++ suite passing said nothing about whether the library was complete,
because completeness is only tested at the point something links against all of it. It also
argues for the Python jobs in CI being genuinely load-bearing rather than a formality -- they
were the only thing standing between this and a broken release.

### The intermittent failures, chased down

A sweep reported one test failing, and single runs would not reproduce it. My first
explanation was that a second `ctest` had been competing for CPU at the time. **That was
wrong**, and worth recording as wrong: it reproduced later with nothing else running. CI then
failed it on three separate jobs, which settled the matter. Two distinct tests were at fault,
both of them mine rather than the engine's.

**1. `FlushTest.NewerTablesShadowOlderOnes` and `ReadsSpanMemtableImmutableMemtableAndL0`
asserted a timing-dependent file count.** Both guard against passing vacuously by requiring
more than one table to exist. With compaction now running automatically, the background
thread could merge those tables into a single L1 file before the assertion ran -- correct
engine behavior, which made the guard fail. Reproduced in 1 of 3 ASan runs; the message was
`actual: 1 vs 1`.

Worse than the flakiness: when it *passed*, it was no longer testing what its name claims.
With everything in one file there is no newer table shadowing an older one. Both tests now
hold automatic compaction off with a high trigger, so several L0 tables genuinely exist and
cross-table shadowing is what gets verified. 15/15 under ASan afterwards.

**2. `CompactionTest.ReadersAreUnaffectedByAConcurrentCompaction` failed its own vacuity
guard.** Reproduced on run 33 of 40. The message was the giveaway: `reads` was zero. **No key
was ever lost** -- the three compactions simply finished before the four reader threads were
scheduled, so `stop` was set before a single read completed, and the guard that exists to stop
the test passing vacuously failed instead.

The test had a second, worse defect found while fixing the first: `ASSERT_TRUE` on the
compaction result in the main thread. An `ASSERT` returns from the test body immediately,
which would leave `stop` unset, the reader threads running against locals about to be
destroyed, and a joinable `std::thread` whose destructor calls `std::terminate`. A failing
compaction would therefore have crashed the suite instead of reporting a failure. It is now an
`EXPECT` with the status captured, falling through to the cleanup, plus a bounded wait for real
reads before stopping. 40/40 under ASan afterwards.

**The lesson across both.** Neither was an engine defect, and that is not reassuring on its
own -- a flaky test is still a broken test, because it trains you to re-run instead of read.
What made them hard was that each failed for a reason unrelated to what it was testing: one
because the engine did its job too well, the other because threads had not started yet. Both
now depend on behavior rather than on timing, and the stress runs that confirm it are recorded
in [NUMBERS.md](NUMBERS.md).

Also worth keeping: the first sweep captured only the summary line, not the failing test's
name, which cost a round of guessing. The sweep now records names.

### Five questions

1. `LogAndApply` writes the MANIFEST record and syncs it *before* installing the new version.
   What state is on disk if the process dies between those two steps, and why is it safe?
2. A read captures snapshot 15. A write then lands at sequence 20, and a compaction runs.
   What would go wrong without the read-snapshot registry, and why is the symptom worse than
   a crash?
3. Compaction output is split at ~8 MB, but only when a new user key begins. What breaks if
   it splits mid-key?
4. `RunCompaction` calls `version.reset()` before asking for obsolete files to be deleted.
   What happens without that line, and why is the mechanism not at fault?
5. Why does a fresh MANIFEST get written on every open instead of appending to the existing
   one forever?

#### Answers

1. The MANIFEST holds the new edit but `current_` still points at the old version. Nothing
   is lost: the next open replays the MANIFEST and arrives at the new version. The reverse
   order would be unsafe -- a version referencing files the MANIFEST does not list would
   vanish on restart, and the files it added would look like garbage and be deleted.
2. Compaction would see version 20 shadowing version 10 and drop version 10. The read then
   seeks to `(K, 15)`, skips version 20 as too new, finds nothing below, and reports
   **NotFound for a key that exists**. It is worse than a crash because nothing fails: the
   database returns a confident wrong answer.
3. Both output files would contain the same user key, so their key ranges overlap. L1 files
   are required never to overlap, and the read path binary searches L1 on that assumption --
   so it would find one file, miss the version in the other, and silently return stale data
   or nothing.
4. The local `shared_ptr` keeps the old version alive, its files count as live, and the files
   the compaction just made obsolete are spared -- disk usage grows with every compaction.
   The mechanism is working exactly as intended: holding a Version is how a reader protects
   its files. The caller simply has to stop being a reader before asking for cleanup.
5. An appended MANIFEST grows without bound across restarts, and recovery replays all of it,
   so every open gets slower forever. A fresh MANIFEST starts with one snapshot record
   describing the whole current state, so recovery cost stays proportional to the data rather
   than to the number of times the database has been opened.

### Gate

Suspended; see [STUDY.md](STUDY.md).

---

## P4 — Concurrency and group commit

### What was built

Writers no longer take turns paying for their own `fsync`. They queue, and the one at the
front becomes the leader: it merges the batches waiting behind it into a single log record,
issues one `fsync` for all of them, inserts them into the memtable, and then hands each
waiting thread its result. The lock is released across the append and the sync, so writers
arriving during it form the next group -- which is why throughput rises with load rather
than flattening. An `Env` fault-injection hook finally makes the flush and compaction
failure branches testable, closing a gap both the P2 and P3 debriefs had flagged as
reviewed-but-unexecuted.

### The files that matter

| File | What it does |
| --- | --- |
| [src/db_impl.cc](../src/db_impl.cc) | `Write` and `BuildBatchGroup`: the queue, the leader handover, the merge rules, and the unlocked I/O section |
| [src/db_impl.h](../src/db_impl.h) | The `Writer` struct and the queue. A writer lives on its own thread's stack; others reach it through the pointer |
| [src/env.cc](../src/env.cc) | Fault injection, so an I/O failure can be produced on demand at exactly one path |
| [tests/cpp/test_group_commit.cc](../tests/cpp/test_group_commit.cc) | Correctness first -- a writer's result is produced by another thread -- then the 60-second stress and the throughput measurement |

### Decisions the spec did not dictate

| Decision | Chose | Alternative considered | Why the alternative lost |
| --- | --- | --- | --- |
| A follower that asked for a sync, behind a leader that did not | Stop the group there; that writer leads the next one | Fold it in anyway | Folding it in would silently give it weaker durability than it asked for. The reverse is fine: a writer that did not ask for a sync may be swept into one that does, since stronger durability harms nobody |
| Group size | Capped at 1 MB, tightened to 128 KB when the leader's own batch is small | No cap | Without a cap, one enormous batch at the front makes every short write behind it wait for megabytes of I/O. The second cap stops a small write being delayed by a group that grew behind it |
| Waking writers | `notify_one` on each writer's own condition variable | One shared variable and `notify_all` | A wake-up reaches exactly the thread it concerns instead of stampeding every waiter, which matters at eight writers and matters more at eighty |
| `enable_group_commit` option | Added | Measure against the previous commit | A flag makes the comparison a test that runs on demand and cannot drift, rather than a number someone has to reproduce by checking out old code. It exists to be measured, not because anyone should run with it off |
| Where the leader inserts into the memtable | Outside the lock, with raw pointers captured under it | Inside the lock | The insert is the second-slowest thing in the write path. Capturing the pointers under the lock makes the safety argument unnecessary rather than merely true -- and the queue already guarantees exactly one leader, which is the skip list's contract |
| Fault injection | On `Env`, matching a path substring, with a bounded count | A mock Env, or a compile-time flag | `Env` is the only place the engine touches the OS, so one hook covers every path that can fail. Bounding the count stops an armed failure leaking into later tests, and matching on a substring means a test can fail a table write without failing the log |
| Stress duration | 60 seconds, overridable by `STRATAKV_STRESS_SECONDS` | Hardcode 60, or pick something shorter | 60 seconds is the acceptance criterion and is the default, so the bar is met by default. The override exists only to keep local iteration bearable; CI never sets it |

### Measured numbers

With 8 writers and `kSyncEveryWrite`, so every write really reaches the disk:

| | writes/sec |
| --- | --- |
| Group commit off | 372 |
| Group commit on | **1,765** |
| **Speed-up** | **4.74x** |

The requirement was 3x. The release build measured 4.72x -- essentially identical, which is
the expected result and worth noting: the measurement is dominated by `fsync` latency, not
by CPU, so optimization does not move it. Working backwards, 372 writes/sec across 8 writers
each doing their own sync implies about 2.7 ms per `F_FULLFSYNC` on this disk, which is a
believable figure for a real cache flush.

The 60-second stress under ThreadSanitizer: **76,538 writes and 5,840,860 reads, zero
reports**, with compaction running throughout.

### Bugs that took more than one attempt

None in this phase, and this time that is worth a sentence rather than a shrug. Group commit
is the most intricate code in the project -- a writer's work is done by another thread, the
lock is dropped halfway through, and the single-writer contract the skip list depends on is
upheld by nothing more than queue position. It went in without a correctness bug because the
three invariants were settled before any code was written:

1. Only `writers_.front()` proceeds, so there is exactly one leader, which is exactly the
   skip list's one-writer rule.
2. A leader stays at the front until it pops itself, so no second leader can appear while the
   lock is released.
3. Everything the unlocked section touches is captured under the lock first, so "is this
   access safe?" never has to be argued at all.

Writing those down first is why the 216 existing tests passed on the first run with the new
write path, and why the 60-second TSan stress was clean the first time.

### Bugs in the tests, including the same mistake twice

The engine code went in clean. My tests did not, and one of them is worth recording because
it repeated a mistake I had fixed hours earlier.

**`AFailedCompactionStopsAcceptingWrites` failed in the release build.** It wrote data,
waited for background work, armed a single injected failure, then asked for a compaction and
expected it to fail. But with automatic compaction on, the background thread may already
have drained L0 by then -- so the requested compaction had nothing to do, succeeded, and the
test failed for a reason unrelated to what it was testing.

That is **exactly** the class of bug I had just finished fixing in three P3 tests: asserting
something that depends on background-thread timing rather than on behavior. Having named the
pattern in the P3 debrief, I then wrote it again the same session. The fix is the same one:
hold automatic compaction off so there is guaranteed work to fail, and assert there are L0
files before arming the failure. 10/10 afterwards in both debug and release.

The lesson is sharper the second time. In a database with background threads, **any test that
observes a count, a file, or a level is implicitly racing the background thread** unless it
has deliberately stopped it. "Wait for background work, then assert" is not sufficient,
because waiting for background work is also waiting for it to have *changed* things. The
default for a test that needs a particular on-disk shape should be to disable the automation
and construct that shape explicitly.

`AFailedFlushStopsAcceptingWrites` had a milder version of the same problem -- a compaction
could have consumed the single armed failure instead of the flush -- and got the same
treatment, plus a tightened assertion that writes must actually be refused rather than the
loose "one of these two things went wrong".

**And a third, found by CI: my throughput test was asserting a property of the disk.**
`EightWritersAreAtLeastThreeTimesFasterWithGroupCommit` passed locally at 4.74x and failed
on the macOS CI runner at **1.40x**. The engine was not at fault. The runner's baseline ran
at 2,159 writes/sec against 372 on this machine -- about 0.46 ms per sync rather than
2.7 ms -- which means `F_FULLFSYNC` there is absorbed by a host cache instead of reaching a
platter. With `fsync` nearly free there is nothing to amortize, so the speed-up collapses.

The wrong fix would have been to lower the threshold until CI passed, which would have left
a number that means nothing. The right fix was to notice that the test was measuring the
**disk** and split the claim in two:

- **The mechanism**, which is hardware-independent: group commit issues *fewer syncs* for the
  same work. `GroupCommitIssuesFarFewerSyncsForTheSameWork` counts them -- 1,200 writes cost
  1,200 syncs without grouping and 258 with it -- and that is what CI gates on, because
  counting operations gives the same answer everywhere.
- **The benefit**, which is not: the timing test prints its numbers on every machine but only
  asserts the 3x ratio where the baseline shows at least 1 ms per sync. Below that it skips
  with a message saying why.

This needed an `Env` sync counter to implement, which was cheap, and it is a better test than
the one it replaces: the sync count states the algorithm's claim directly, where the timing
was a proxy for it that happened to correlate on my hardware. The spec asks for the 3x to be
"measured and recorded in NUMBERS.md", and 4.74x on a named machine is still what is recorded
there -- the assertion simply no longer pretends to be reproducible on hardware that cannot
reproduce it.

**And a fourth, the worst of the session: I broke the Linux build and pushed it.**
`src/env.cc` used `std::atomic` without including `<atomic>`. It compiled cleanly on macOS,
because libc++ happens to pull `<atomic>` in through another header; GCC's libstdc++ does
not, so **every Linux job failed to build** -- five C++ jobs and two Python jobs -- in about
twenty seconds each, while every macOS job passed. All 228 local tests passed throughout.

Two separate failures of mine produced it:

1. The patch that was supposed to add the include targeted the text
   `"#include <cerrno>\n#include <mutex>"`, which is not what the file contains -- there are
   three other includes between them. `str.replace` does nothing when the pattern is absent
   and reports nothing, so the include was silently never added. Every patch of this kind now
   asserts that it matched.
2. Relying on one standard library's transitive includes, which is relying on luck.

An audit then found **twelve more** latent cases, all masked the same way: `std::array` in
`version.cc`, `std::unique_ptr` in `wal_reader.cc`, `size_t` without `<cstddef>` in seven
files. Any of them could have broken a future build on a different compiler or library
version, for reasons that would have looked unrelated to whatever change happened to expose
them.

[tools/check_includes.py](../tools/check_includes.py) now checks all 42 source files and runs
in the CI lint job, so the class of bug cannot reach a push again. That is the real fix --
the thirteen individual includes are just today's instances.

This is the second time this session that a missing definition or include passed the entire
C++ suite and was caught only by a different toolchain. The pattern is worth naming: **a
local green run says the code works with one compiler and one standard library.** It says
nothing about portability, and portability is not a nice-to-have when the CI matrix is part
of the acceptance criteria.

**A tooling note.** My sweep command reported asan as having failures when it had none: the
grep that extracts failing test names also matched ctest's list of *skipped* tests. Two tests
are deliberately skipped under the sanitizers, so a clean run looked dirty. The pattern now
excludes skips. Worth recording because for a minute I believed a passing run had failed,
which is the opposite of the usual danger but just as misleading.

### Five questions

1. Eight threads call `Put` simultaneously. How many `fsync`s happen, and whose thread does
   the work for the other seven?
2. The leader releases the lock before the append and the `fsync`. What stops a second thread
   becoming a leader and writing to the memtable at the same time?
3. A writer that asked for `sync` arrives behind a leader that did not. Why can it not simply
   be added to the group?
4. Why does group commit get *faster* as more threads write, rather than just not getting
   slower?
5. What is `enable_group_commit` for, given nobody should ever turn it off?

#### Answers

1. One, in the best case. The first thread to reach the front becomes the leader, merges the
   other seven batches into its own record, and does a single append and a single `fsync`. The
   other seven sleep, then wake to find their `status` already filled in by the leader's
   thread and return it as their own.
2. Queue position. Only `writers_.front()` leaves the wait, and the leader stays at the front
   until it pops itself at the very end. So there is exactly one leader at any moment, which
   is precisely the single-writer contract the skip list requires -- no extra lock needed.
3. Because the group is not going to call `fsync`, so that writer would be told its write
   succeeded while the record is still only in the page cache -- a weaker guarantee than it
   asked for. The group stops at that writer instead, and it leads the next group and gets
   its own sync. The opposite direction is harmless: being swept into a group that syncs gives
   a writer more durability than it requested.
4. Because the queue fills while the leader is inside the slow `fsync`. More concurrent
   writers means more batches waiting when the leader finishes, so the next group is larger
   and the per-write cost of the shared `fsync` falls. The thing that would otherwise be pure
   contention becomes the source of the speed-up.
5. Measuring. It exists so the 4.7x figure is produced by a test that runs on demand and
   compares two live code paths, rather than being a number someone has to reproduce by
   checking out an old commit. A claim on a resume should be re-runnable.

### Gate

Suspended; see [STUDY.md](STUDY.md).

---

## P5 — Python package, property tests, and the crash fuzzer

### What was built

The engine became usable from Python, and then got tested far harder than any hand-written
suite could manage. The binding layer exposes `get`, `put`, `delete`, `write_batch` and
`close`, plus mapping syntax and a context manager; it releases the GIL around every engine
call and turns each `Status` into the exception a Python caller would expect. Hypothesis
then drives the engine and a plain `dict` through thousands of random operation sequences,
checking after every step that they agree, with reopens mixed in so recovery runs constantly.
And the crash fuzzer `SIGKILL`s a writing process at random moments -- sometimes inside a
named crash point -- reopens the database, and demands every acknowledged write back.

### The files that matter

| File | What it does |
| --- | --- |
| [python/bindings.cc](../python/bindings.cc) | The GIL handling and the Status-to-exception mapping. Every key is copied into C++ memory *before* the GIL is released, because Python objects may not be touched without it |
| [tests/py/crash_fuzz.py](../tests/py/crash_fuzz.py) | The project's central claim, tested the only way it can be: kill the process and demand the promises back |
| [tests/py/fuzz_writer.py](../tests/py/fuzz_writer.py) | Defines "acknowledged" operationally -- it prints a key only after the write returned, and flushes immediately |
| [tests/py/test_stateful.py](../tests/py/test_stateful.py) | The engine against a dict, over random sequences, with reopens |
| [python/stratakv/\_\_init\_\_.py](../python/stratakv/__init__.py) | The typed wrapper: mapping syntax, context manager, and the KeyError-vs-None distinction |

### Decisions the spec did not dictate

| Decision | Chose | Alternative considered | Why the alternative lost |
| --- | --- | --- | --- |
| `MutableMapping` | Implement the mapping dunders but do **not** inherit from the ABC | Inherit, as the spec's snippet shows, and raise `NotImplementedError` from `__iter__`/`__len__` | The ABC requires `__iter__` and `__len__`, which need range scans this engine does not have. Inheriting would make `dict(db)` and `len(db)` type-check and then fail at runtime -- advertising an interface it cannot honor. When range scans land, both can be added and the ABC claimed honestly. See the objection below |
| `delete()` versus `del db[k]` | `delete()` is idempotent; `del db[k]` raises `KeyError` | Make both behave the same | They answer to different contracts. The engine's delete writes a tombstone and deleting nothing is meaningless, not erroneous. But `del` on a mapping raises `KeyError`, and a Python caller will expect that. Keeping both costs one documented sentence and a read before the delete in the dunder only |
| I/O failures | `OSError` | A dedicated `StrataKVIOError` | `OSError` is what a Python caller already catches for a failed file operation. A new type would only make it easier to miss |
| Corruption | A dedicated `CorruptionError`, under a `StrataKVError` base | Fold into `OSError` | Corruption means the bytes arrived and were wrong; an I/O error means they never arrived. That difference decides what a caller does next, so it should not be collapsed |
| `str` keys | Rejected with `TypeError` | Encode them as UTF-8 | Silently encoding would make `b"k"` and `"k"` collide in a store whose keys are bytes. A loud failure is kinder than a subtle one |
| Fuzzer kill timing | A random 0.20–0.60 s | A shorter range, which is what I first wrote | Starting Python, importing the extension and opening the database costs well over a tenth of a second, so a shorter sleep kills the child before it has acknowledged anything. See the struggles below |
| Stateful test configuration | Two machines: one with a roomy memtable for the long run, one with a tiny one for flush-heavy coverage | A single compromise setting | They buy different things, and the tiny setting is 42x slower per sequence. One setting would have meant either no flush coverage or a two-hour acceptance run |

### An objection, as the spec asks for one

The spec's snippet 9 shows the wrapper as `class DB(MutableMapping[bytes, bytes])`, and the
C++ primer describes "the `MutableMapping` interface so `db[k] = v` works like a dict".

**The objection:** `collections.abc.MutableMapping` requires `__iter__` and `__len__`. This
engine has no range scans -- they are a P6 stretch goal -- so neither can be implemented.
Inheriting anyway and raising `NotImplementedError` would mean `dict(db)`, `len(db)`,
`db.keys()` and `db.items()` all type-check against the ABC and then fail at runtime. A type
that claims an interface it cannot honor is worse than one that claims less.

**What was built instead:** the mapping methods that *can* be honored -- `__getitem__`,
`__setitem__`, `__delitem__`, `__contains__` -- without the ABC. `db[k] = v` works like a
dict, which is what the primer actually wanted. `test_not_a_mutable_mapping` pins the
decision so it is deliberate rather than an omission, and the class docstring explains it.

**What would change it:** range-scan iterators. With those, `__iter__` and `__len__` become
implementable and the ABC can be claimed truthfully.

### Bugs that took more than one attempt

**1. The stateful test was 42 times slower than it needed to be, because of one constant.**
I set `memtable_bytes=4096` to force rotations and flushes during every example. 300
sequences took three minutes, which extrapolated to nearly two hours for the ten thousand
the acceptance criterion asks for.

I nearly accepted that as the cost of thorough testing. Measuring instead of assuming showed
what was really happening: the arena hands out 4 KiB blocks and a memtable's reported usage
counts *allocated blocks*, so a 4 KiB limit is crossed by the very first write. The engine
was rotating and flushing on nearly every single write. Measured directly: **894 ms per
sequence at 4 KiB against 21 ms at 32 KiB.**

The slowness was my configuration interacting with an implementation detail, not the engine
being slow. The fix was to stop compromising: a roomy memtable for the ten-thousand-sequence
run, and a second machine that keeps the flush-on-every-write coverage at a twentieth of the
sequences. The lesson is the one I keep relearning -- a surprising number is worth one
measurement before it becomes an accepted cost.

**2. The first fuzzer verified nothing in half its iterations.** The initial kill delay was a
random 0.02–0.35 s. Starting Python, importing the extension and opening a database takes
well over 0.1 s, so a large fraction of iterations killed the child before it had
acknowledged anything -- and an iteration that verifies nothing proves nothing. The summary
line said "8 iterations, 107 acknowledged writes" and looked fine, which is what made it
worth catching.

Two changes. The floor went up to 0.20 s. And the fuzzer now distinguishes the two reasons an
iteration can come up empty: `write_before_wal` and `write_after_wal_sync` fire on the *first*
write, so they can never contribute verified writes and that is expected; anything else coming
up empty means the kill was too early. **The run now fails outright if more than half its
iterations were killed too early**, so a future change cannot quietly make the fuzzer vacuous
while still printing a reassuring number. In the 1,000-iteration run: 288 empty by design, 1
too early.

### Measured numbers

The acceptance criteria, both met and recorded in [NUMBERS.md](NUMBERS.md):

- **Crash fuzzer, 1,000 iterations: 43,088 acknowledged writes verified, zero lost, zero
  corruption errors.** 501 killed by raw `SIGKILL`, 499 by an armed crash point, all seven
  points exercised 124–160 times each.
- **Hypothesis: 10,000 random operation sequences** against a dict model, plus 500 more with
  a memtable small enough to flush on nearly every write. Up to 50 operations per sequence,
  with reopens mixed in, so on the order of half a million operations in total. 49 minutes.

The 49 minutes are a finding in their own right. The `reopen` rule means roughly 80,000
database opens, and an open does six to eight real `F_FULLFSYNC` calls regardless of the
database's sync policy -- metadata durability does not follow the write policy, which is
correct but means an open-and-close-heavy workload pays far more than its write count
suggests. Reducing it is a legitimate optimization, not a test problem.

CI runs a 50-iteration fuzz and the full Python suite on every push.

### Five questions

1. In the binding, why must the key be copied into a `std::string` *before*
   `py::gil_scoped_release`, rather than after?
2. The fuzzer child prints each key only after `put` returns, and flushes immediately. What
   goes wrong if it prints before the write, and what goes wrong if it does not flush?
3. Why does the fuzzer check only acknowledged writes rather than every key the child tried
   to write?
4. `db.delete(k)` on a missing key is fine, but `del db[k]` raises `KeyError`. Why the
   difference, and what does the dunder cost that `delete` does not?
5. Why is `stratakv.DB` deliberately not a `collections.abc.MutableMapping`?

#### Answers

1. Because a Python object may only be touched while holding the GIL, and `key` is a Python
   object. After the release there is no GIL, so reading it would be undefined behavior --
   the kind that usually works and occasionally corrupts memory. The copy puts the bytes in
   C++ memory that the engine can read safely without it.
2. Printing *before* the write would make the parent demand keys that were never promised, so
   the fuzzer would report failures for writes that were legitimately in flight -- it would
   fail on correct behavior. Not flushing would lose buffered lines to the `SIGKILL`, so the
   parent would *not* demand writes that really were promised -- the fuzzer would silently
   check less than it claims, which is worse because it still looks like it passed.
3. Because a write still in flight at the kill may or may not have survived, and **both
   outcomes are correct**: the caller was never told it succeeded. The guarantee is about
   acknowledged writes specifically, so checking unacknowledged ones would fail the engine for
   behavior it is allowed to have.
4. They answer to different contracts. The engine's delete appends a tombstone, and doing that
   for a key that is not there is meaningless rather than wrong -- so it is idempotent. But
   `del` on a mapping raises `KeyError` in Python, and a caller will expect that. The dunder
   therefore does a read first to find out whether the key exists, which `delete` does not
   need and does not pay for.
5. The ABC requires `__iter__` and `__len__`, which need range scans the engine does not have
   yet. Inheriting and raising `NotImplementedError` would let `dict(db)` and `len(db)`
   type-check and then fail at runtime, which is worse than not claiming the interface. The
   useful dunders are implemented without it, and the ABC can be claimed honestly once range
   scans exist.

### Gate

Suspended; see [STUDY.md](STUDY.md).
