# StrataKV

An embedded key-value storage engine built on a log-structured merge tree (LSM tree).
C++20 engine, Python package, crash fuzzer, and benchmarks. In one sentence: a tiny
RocksDB — you `put`, `get`, and `delete` keys, every acknowledged write survives
`kill -9`, and reads stay fast as data grows past memory.

> **Status: P5 of 7 complete.** Every acknowledged write reaches a checksummed
> write-ahead log and is `fsync`ed before `Put` returns; full memtables are flushed to
> immutable SSTables on a background thread, and reads span memtable, immutable memtable,
> and L0 with bloom filters sparing most lookups any disk read. Compaction now merges L0
> into L1, reclaiming space from overwrites and deletes, with a MANIFEST recording the file
> set and reference-counted version snapshots so files are never deleted under a running
> read. Leader-follower group commit shares one `fsync` across queued writers, measured
> **4.7× faster** with 8 concurrent writers. The Python package is complete, and the crash
> fuzzer has verified **43,088 acknowledged writes across 1,000 SIGKILL crashes with zero
> lost**. SQLite benchmarks land in P6 — every number here comes from
> [docs/NUMBERS.md](docs/NUMBERS.md).

## What works today

| Piece | Where | What it does |
| --- | --- | --- |
| `Status` | [include/stratakv/status.h](include/stratakv/status.h) | Error returns instead of exceptions: `Ok`, `NotFound`, `Corruption`, `IOError`, `InvalidArgument` |
| `Options` | [include/stratakv/options.h](include/stratakv/options.h) | Memtable size and sync policy |
| `Env` | [src/env.h](src/env.h) | Every syscall the engine makes: append, `fsync`, rename, directory `fsync`, `flock` |
| `crc32c` | [src/crc32c.h](src/crc32c.h) | Hand-written CRC32C, compile-time table, no dependency |
| `stratakv.version()` | [python/stratakv/\_\_init\_\_.py](python/stratakv/__init__.py) | Proves the full build chain: CMake → pybind11 → wheel → `import stratakv` |
| `DB` | [include/stratakv/db.h](include/stratakv/db.h) | `Open`, `Put`, `Get`, `Delete`, `Write(batch)`, `Close`, with a `LOCK` file so two processes cannot open one database |
| `WriteBatch` | [include/stratakv/write_batch.h](include/stratakv/write_batch.h) | A group of writes that becomes one WAL record with one checksum, so it replays entirely or not at all |
| WAL | [src/wal_writer.h](src/wal_writer.h), [src/wal_reader.h](src/wal_reader.h) | Checksummed records; a torn tail is truncated, mid-file damage is reported as `Corruption` |
| Memtable | [src/memtable.h](src/memtable.h), [src/skiplist.h](src/skiplist.h) | Skip list with one writer and any number of lock-free readers |
| Crash points | [src/crash_point.h](src/crash_point.h) | `STRATAKV_CRASH_AT=<name>` kills the process at a named moment in the write or flush path |
| SSTables | [src/sstable_builder.h](src/sstable_builder.h), [src/sstable_reader.h](src/sstable_reader.h) | Sorted immutable files: data blocks each with their own CRC32C, bloom filter, index, checksummed footer |
| Bloom filters | [src/bloom.h](src/bloom.h) | 10 bits/key, 7 probes, **measured 0.729%** false-positive rate |
| Background flush | [src/db_impl.h](src/db_impl.h) | Memtable rotation at 4 MB; a log is deleted only once its table is durable |
| MANIFEST + versions | [src/version.h](src/version.h) | Append-only log of version edits, atomic `CURRENT` replacement, reference-counted snapshots |
| Compaction | [src/compaction.h](src/compaction.h) | L0→L1 k-way merge; drops shadowed versions and tombstones, **1.445×** space amplification measured |
| Group commit | [src/db_impl.h](src/db_impl.h) | Leader merges queued batches into one record with one shared `fsync`: **4.74×** at 8 writers, TSan-clean over a 60 s 8×8 stress |
| Python package | [python/stratakv/\_\_init\_\_.py](python/stratakv/__init__.py) | `DB` with mapping syntax and a context manager; releases the GIL during engine calls |
| Crash fuzzer | [tests/py/crash_fuzz.py](tests/py/crash_fuzz.py) | `SIGKILL`s a writing process at random moments and in named crash points, then demands every acknowledged write back |

## Building

Requires CMake 3.24+, a C++20 compiler (GCC 12+ or Clang 15+), and Ninja.

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Four presets are defined. Correctness is checked under all of them, not just `debug`:

| Preset | What it is for |
| --- | --- |
| `debug` | Symbols, no optimization — the default for development |
| `release` | Optimized; the only preset benchmarks may be run under |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer — memory errors and UB |
| `tsan` | ThreadSanitizer — data races |

## Installing the Python package

```bash
pip install .
python -c "import stratakv; print(stratakv.version())"
```

`pip install .` runs CMake through scikit-build-core, compiles the extension, and places
it inside the `stratakv` package. Run the Python tests with `pip install '.[test]'` then
`pytest`.

## Formatting and linting

The linters are pinned in [requirements-lint.txt](requirements-lint.txt) so that a local
run and a CI run are the same check — clang-format versions disagree with each other about
real formatting decisions, so the version is fixed rather than left to whatever the OS
ships.

```bash
pip install -r requirements-lint.txt
find include src tests/cpp python \( -name '*.cc' -o -name '*.h' \) -print | xargs clang-format -i
./tools/run-clang-tidy.sh build/debug
ruff check python tests/py tools && ruff format python tests/py tools
```

## Crash-safety guarantees

The engine's one central claim: **if `Put` returned OK, that write survives a crash.**

- A write reaches the write-ahead log and is `fsync`ed *before* it is acknowledged.
  Everything after that — memtable rotation, flush, compaction — happens in the background.
- On macOS, `fsync` alone does not flush the drive's own write cache, so
  `Env::Sync` uses `fcntl(fd, F_FULLFSYNC)`. See [src/env.cc](src/env.cc).
- A torn record at the tail of the WAL is normal after a crash and is never an error:
  it was never acknowledged. Recovery truncates it.
- A failed `fsync` is treated as permanent rather than retried, because a retry can
  report success after the kernel has already discarded the data.

How this gets proven, phase by phase: WAL replay tests with a deliberately truncated
tail (P1), crash points during flush (P2) and compaction (P3), and a fuzzer that
`SIGKILL`s a writing process at random moments and verifies every acknowledged key
survives (P5).

## Architecture

Writes go to a write-ahead log first, then into memory; memory is periodically flushed to
immutable sorted files on disk; a background thread merges those files. Reads check the
newest data first and stop at the first match.

```
Put(k, v) ──> WAL append ──> fsync ──> memtable insert ──> acknowledge
                                           │
                        (4 MB) ────────────┘
                           │
                           v
                   immutable memtable ──flush──> L0 SSTable
                                                    │
                                   (4 files) ──compact──> L1 SSTables
```

Reads walk: active memtable → immutable memtable → L0 files newest to oldest → the one L1
file whose key range covers the key. Each SSTable checks its bloom filter before touching
disk.

## Documentation

- [docs/STUDY.md](docs/STUDY.md) — **start here to learn the codebase**: the phase-by-phase study path
- [docs/DESIGN.md](docs/DESIGN.md) — on-disk formats and the decisions behind them
- [docs/DEBRIEF.md](docs/DEBRIEF.md) — what each phase built, and what it cost
- [docs/NUMBERS.md](docs/NUMBERS.md) — every measured number, with the command that produced it
- [docs/learning/](docs/learning/) — per-phase walkthroughs of the real code
- [docs/CPP_NOTES.md](docs/CPP_NOTES.md), [docs/GLOSSARY.md](docs/GLOSSARY.md) — C++ features and terms as they come up

## Limitations

By design, and not planned: networking, replication, transactions across keys beyond
atomic write batches, and Windows support. Range-scan iterators are a stretch goal.
