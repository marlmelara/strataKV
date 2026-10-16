# Measured numbers

Every number that appears in the README or on a resume must appear here first, with the
exact command that produced it, the machine it ran on, and the date. A number that is not
in this file does not go on the resume; if a clause has no number, the clause gets deleted.

**No performance numbers exist yet.** P0 produced none, and none have been invented. The
first real measurements arrive in P4 (group commit throughput) and P6 (benchmarks against
SQLite).

## Machine profile

Referenced below as **M1** so each row does not have to repeat it.

| Field | Value |
| --- | --- |
| Label | M1 |
| CPU | Apple M2, 8 cores |
| Memory | 8 GB |
| Disk | Internal SSD (APFS) |
| OS | macOS 26.6.2 (arm64) |
| Compiler | Apple Clang 21.0.0 |
| CMake / Ninja | 4.4.4 / 1.13.2 |
| Python | 3.14.3 |

## Correctness results

### P0

These are pass/fail facts, not performance numbers, but they are what the project claims,
so they are recorded the same way.

| Result | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| C++ tests, debug preset | 26/26 passed | `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` | M1 | 2026-10-15 |
| C++ tests, release preset | 26/26 passed | `cmake --preset release && cmake --build --preset release && ctest --preset release` | M1 | 2026-10-15 |
| C++ tests, ASan + UBSan | 26/26 passed, 0 reports | `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` | M1 | 2026-10-15 |
| C++ tests, TSan | 26/26 passed, 0 reports | `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan` | M1 | 2026-10-15 |
| Python tests | 5/5 passed | `pip install '.[test]' && pytest` | M1 | 2026-10-15 |
| CRC32C known-answer, `"123456789"` | `0xE3069283` | `ctest --preset debug -R Crc32c.MatchesKnownAnswers` | M1 | 2026-10-15 |
| clang-tidy, curated checks | 0 diagnostics | `./tools/run-clang-tidy.sh build/debug` | M1 | 2026-10-15 |
| CI, all 13 jobs | passed on Ubuntu and macOS | push to `main`, [run 38786203847](https://github.com/marlmelara/strataKV/actions/runs/38786203847) | GitHub Actions `ubuntu-latest` + `macos-latest` | 2026-10-15 |

### P1

| Result | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| C++ tests, all four presets | 111/111 passed each, 0 sanitizer reports | `for p in debug release asan tsan; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p; done` | M1 | 2026-10-15 |
| 10,000 keys recovered after a torn final WAL record | all complete records recovered | `ctest --preset debug -R DBTest.TenThousandKeysSurviveATornFinalRecord` | M1 | 2026-10-15 |
| One corrupt byte in a middle WAL record | detected, open refused | `ctest --preset debug -R DBTest.OneCorruptByteInAMiddleRecordIsDetected` | M1 | 2026-10-15 |
| Lock-free readers alongside one writer | 0 TSan reports | `ctest --preset tsan -R SkipListTest.ReadersScanSafelyWhileOneWriterInserts` | M1 | 2026-10-15 |
| clang-tidy, curated checks | 0 diagnostics | `./tools/run-clang-tidy.sh build/debug` | M1 | 2026-10-15 |

### P2

| Result | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| C++ tests, all four presets | 158/158 passed each, 0 sanitizer reports | `for p in debug release asan tsan; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p; done` | M1 | 2026-10-15 |
| Bloom filter false-positive rate | **0.729%** (729 of 100,000 absent keys; 10 bits/key, 7 probes, 10,000 keys added) | `./build/debug/tests/cpp/stratakv_tests --gtest_filter='Bloom.FalsePositiveRateIsAboutOnePercent'` | M1 | 2026-10-15 |
| 1,000,000 keys written, reopened, all read back correctly | passed in 12.4 s (default 4 MB memtable, batched, `kNoSync`) | `ctest --preset debug -R OneMillionKeysSurviveReopen` | M1 | 2026-10-15 |
| Crash during flush, every acknowledged write recovered | passed for `flush_after_sst_write` and `flush_before_wal_delete` | `ctest --preset debug -R AcceptanceCrashPoints` | M1 | 2026-10-15 |
| clang-tidy, curated checks | 0 diagnostics | `./tools/run-clang-tidy.sh build/debug` | M1 | 2026-10-15 |
| Concurrent reads during repeated flushes | 20/20 consecutive runs, 0 TSan reports | `for i in $(seq 1 20); do ctest --preset tsan -R 'Flush\|CrashPoints' -E OneMillionKeys; done` | M1 | 2026-10-15 |

### P3

| Result | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| 5,000,000 operations vs a reference `std::map` | matched exactly across 360,035 surviving keys | `ctest --preset debug -R MillionsOfOperationsMatchAReferenceMap` | M1 | 2026-10-15 |
| Space amplification after compaction | **1.44x** (13,514,830 bytes on disk, 9,360,910 bytes live) | same test, prints the ratio | M1 | 2026-10-15 |
| 5,000,000-operation run time | 35.2 s | same test | M1 | 2026-10-15 |
| Crash during compaction, every acknowledged write recovered | passed for `compaction_after_output` and `compaction_before_manifest` | `ctest --preset debug -R AcceptanceCompactionCrashPoints` | M1 | 2026-10-15 |
| clang-tidy, curated checks | 0 diagnostics | `./tools/run-clang-tidy.sh build/debug` | M1 | 2026-10-15 |
| Reads and compactions overlapping | 15/15 consecutive runs, 0 TSan reports | `for i in $(seq 1 15); do ctest --preset tsan -R 'Readers\|Compaction' -E Millions; done` | M1 | 2026-10-15 |

| Flaky-test fixes, stress-verified | `ReadersAreUnaffectedByAConcurrentCompaction` 40/40 (was ~1 in 33); `NewerTablesShadowOlderOnes` + `ReadsSpanMemtableImmutableMemtableAndL0` 15/15 (was ~1 in 3) | repeated `./build/asan/tests/cpp/stratakv_tests --gtest_filter=...` | M1 | 2026-10-15 |

On the 1.44x: the requirement is 1.5x, so the margin is thin and worth understanding rather
than celebrating. The denominator counts only raw key and value bytes, while the files also
carry an 8-byte internal-key trailer per entry (about 2.9 MB over 360,035 keys), plus a bloom
filter, an index, and a footer per file, plus any L0 files not yet compacted. Most of the
overhead is the format itself rather than uncollected garbage, which also means more
aggressive compaction would not move the figure much.

The 5,000,000-operation test is skipped under ASan and TSan. It is a volume test, and
instrumented it would add roughly half an hour to every sweep; the tests that are about
races and memory errors run under both sanitizers at a size that finishes.

The measured 0.729% sits just under the ~0.8% the theory predicts for 10 bits per key with
7 probes, which is the expected result: the filter is slightly larger than the exact
bits-per-key figure because its size is rounded up to whole bytes.

The CI row is what satisfies P0's third acceptance criterion. It covers all four presets on
both operating systems, the Python package on 3.11 and 3.13, and the lint job. Note that
the sanitizer results above were produced on Apple Silicon; the Ubuntu x86-64 runners
exercise the same presets, which matters because ARM reorders memory more aggressively than
x86 and the skip list in P1 will depend on that distinction.

## Performance numbers

### P4 — group commit

All with 8 writer threads and `kSyncEveryWrite`, so every write really reaches the disk and
the `fsync` cost is what is being shared.

| Metric | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| Durable writes/sec, 8 writers, group commit **off** | 372 | `./build/debug/tests/cpp/stratakv_tests --gtest_filter='GroupCommitTest.EightWritersAreAtLeastThreeTimesFasterWithGroupCommit'` | M1 | 2026-10-15 |
| Durable writes/sec, 8 writers, group commit **on** | **1,765** | same test | M1 | 2026-10-15 |
| **Group commit speed-up** | **4.74x** | same test | M1 | 2026-10-15 |
| Same measurement, release build | 345 → 1,629, **4.72x** | `./build/release/tests/cpp/stratakv_tests --gtest_filter='GroupCommitTest.EightWriters*'` | M1 | 2026-10-15 |
| 60 s stress, 8 writers + 8 readers, under TSan | 76,538 writes, 5,840,860 reads, **0 reports** | `./build/tsan/tests/cpp/stratakv_tests --gtest_filter='GroupCommitTest.EightWritersAndEightReadersUnderSustainedLoad'` | M1 | 2026-10-15 |
| Syncs for 1,200 durable writes, group commit **off** | 1,200 (one per write) | `./build/debug/tests/cpp/stratakv_tests --gtest_filter='GroupCommitTest.GroupCommitIssuesFarFewerSyncsForTheSameWork'` | M1 | 2026-10-15 |
| Syncs for 1,200 durable writes, group commit **on** | **258** (4.65 writes per sync) | same test | M1 | 2026-10-15 |

The acceptance bar was 3x; the measurement is 4.74x. Two things are worth reading off these
numbers rather than just the ratio:

- **The release build gives the same answer** (4.72x). That is the expected result and a
  useful sanity check: the measurement is dominated by `fsync` latency, not by CPU, so
  compiler optimization does not move it. A speed-up that changed a lot between build types
  would suggest the benchmark was measuring the wrong thing.
- **372 writes/sec across 8 writers each paying for their own sync implies about 2.7 ms per
  `F_FULLFSYNC`** on this disk. That is a believable figure for a flush that really reaches
  the platter, and it is the number group commit is amortizing.

### Why the timing ratio is not the CI gate

The same code measured **1.40x** on a GitHub macOS runner, where the baseline ran at 2,159
writes/sec — about **0.46 ms per sync**, five times cheaper than on real hardware. At that
price `fsync` is being absorbed by a host cache rather than reaching a platter, so there is
nothing for group commit to amortize and the ratio is measuring the runner's disk rather
than this engine.

The timing test therefore prints its numbers everywhere but only asserts the 3x ratio when
the measured baseline shows at least 1 ms per sync. What CI gates on instead is the
**sync count**: 1,200 writes cost 1,200 syncs without grouping and 258 with it. That is the
mechanism stated as a count of operations rather than a span of time, so it holds on any
machine — which is the right shape for an assertion, and the recorded 4.74x stays the
measured claim on a named machine as the spec requires.

Still to come:

- P6: sequential fill, random fill, random read (hit), random read (miss) — at 100 B and
  1 KB values, StrataKV vs SQLite at matched durability settings
- P6: write amplification (bytes written to disk / bytes written by the user)
