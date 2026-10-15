# StrataKV

An embedded key-value storage engine built on a log-structured merge tree (LSM tree).
C++20 engine, Python package, crash fuzzer, and benchmarks. In one sentence: a tiny
RocksDB — you `put`, `get`, and `delete` keys, every acknowledged write survives
`kill -9`, and reads stay fast as data grows past memory.

> **Status: P0 of 7 complete.** The scaffold, the OS layer, and CI are in place. The
> write-ahead log, memtable, SSTables, compaction, and the Python `DB` class land in
> P1–P5. There are no benchmark numbers in this README yet because none have been
> measured — see [docs/NUMBERS.md](docs/NUMBERS.md).

## What works today

| Piece | Where | What it does |
| --- | --- | --- |
| `Status` | [include/stratakv/status.h](include/stratakv/status.h) | Error returns instead of exceptions: `Ok`, `NotFound`, `Corruption`, `IOError`, `InvalidArgument` |
| `Options` | [include/stratakv/options.h](include/stratakv/options.h) | Memtable size and sync policy |
| `Env` | [src/env.h](src/env.h) | Every syscall the engine makes: append, `fsync`, rename, directory `fsync`, `flock` |
| `crc32c` | [src/crc32c.h](src/crc32c.h) | Hand-written CRC32C, compile-time table, no dependency |
| `stratakv.version()` | [python/stratakv/\_\_init\_\_.py](python/stratakv/__init__.py) | Proves the full build chain: CMake → pybind11 → wheel → `import stratakv` |

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
