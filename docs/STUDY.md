# Reading guide

How to get from "I opened this repository" to "I understand why it is built this way."

The engine was built in seven stages, and that order is still the best order to read it in:
each stage is a complete, working thing that the next one extends. For every stage there is
a plain-English walkthrough of the real code in [learning/](learning/), and a section in
[DEBRIEF.md](DEBRIEF.md) covering what was built, the decisions the design did not force,
and the bugs that took more than one attempt.

## The five ideas that carry the design

If you read nothing else, read these. They are the parts where a storage engine stops being
a data structure exercise and starts being about durability.

1. **Where "acknowledged" happens.** A `Put` is a promise only after its `fsync` returns,
   not after the append. Everything else — memtable rotation, flush, compaction — is
   background work that has already happened behind a promise that was kept.
2. **Why a torn tail in the write-ahead log is not an error.** A half-written final record
   is the normal result of a crash and was never acknowledged, so recovery truncates it.
   Telling that apart from corruption in the *middle* of the log is the interesting part.
3. **The four-step atomic file replacement** — write temp, sync temp, rename, sync the
   directory. Drop step 2 and the rename can point at garbage; drop step 4 and the rename
   itself may not survive a crash.
4. **How readers search the skip list without taking a lock.** A node is fully built before
   it is published with a single release store, and readers reach it only through acquire
   loads. Without that pairing a reader on another core can see the pointer before the
   contents — invisible on x86, real on ARM.
5. **Why group commit makes writes faster as contention rises.** A leader merges every
   queued batch into one log record and one `fsync`, so the per-write cost of durability
   falls as more threads write. Measured at 4.74× with 8 writers.

[DESIGN.md](DESIGN.md) covers all five with the on-disk formats.

## Stage by stage

| Stage | What it covers | Walkthrough |
| --- | --- | --- |
| 0 | Build system, `Status`, `Options`, `Env`, CRC32C | [scaffold and CI](learning/P0-scaffold-and-ci.md) |
| 1 | Write-ahead log, memtable, skip list, crash recovery | [WAL, memtable, recovery](learning/P1-wal-memtable-recovery.md) |
| 2 | SSTables, bloom filters, flush, the read path | [SSTables and the read path](learning/P2-sstables-flush-read-path.md) |
| 3 | MANIFEST, version snapshots, compaction, merging iterator | [MANIFEST, versions, compaction](learning/P3-manifest-versions-compaction.md) |
| 4 | Concurrency and group commit | [concurrency and group commit](learning/P4-concurrency-group-commit.md) |
| 5 | Python package, property tests, crash fuzzer | [Python package and fuzzer](learning/P5-python-package-fuzzer.md) |
| 6 | Benchmarks, write amplification, measuring honestly | [benchmarks and honesty](learning/P6-benchmarks-and-honesty.md) |

Each walkthrough opens with a summary under 150 words, traces one operation end to end
through the real files, then annotates the handful of functions that matter line by line.
They are written for someone fluent in Python and new to C++, so the closest Python
equivalent sits next to each piece of unfamiliar syntax.

Each debrief section ends with five questions written against this repository's actual
code — useful whether you are studying it or interviewing someone on it.

## Reference material

Read these when a term or a language feature is unfamiliar, rather than front to back.

| File | What it holds |
| --- | --- |
| [DESIGN.md](DESIGN.md) | On-disk formats and the durability invariants, with the reasoning |
| [DEBRIEF.md](DEBRIEF.md) | What each stage built, the alternatives that lost, and the bugs |
| [NUMBERS.md](NUMBERS.md) | Every measured number and the command that produced it |
| [CPP_NOTES.md](CPP_NOTES.md) | Every C++ feature used in the real code, explained next to Python |
| [GLOSSARY.md](GLOSSARY.md) | Storage, systems, and concurrency terms as they come up |

## If you are evaluating this repository

Three files carry most of what is worth seeing:

- [DEBRIEF.md](DEBRIEF.md) — every significant bug, including the ones that were embarrassing.
  The P6 section documents four benchmark bugs that each made this project look *better* than
  it was, and why that direction of error is the one that survives longest.
- [NUMBERS.md](NUMBERS.md) — every published figure with its command, machine and date,
  including figures that were later found to be measuring the wrong thing and are kept with
  that noted rather than deleted.
- [DESIGN.md](DESIGN.md) — the formats and the durability argument.
