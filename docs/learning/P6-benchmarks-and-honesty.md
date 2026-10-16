# P6 — Benchmarks, write amplification, and measuring honestly

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are in [GLOSSARY.md](../GLOSSARY.md); new C++ syntax is in
[CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

This phase adds no engine features. It measures the engine against SQLite and writes down
what it found — and almost all of the work turned out to be in making the measurement
mean anything.

Four separate bugs in the benchmark itself each produced a *flattering* number, and each
one looked completely plausible until it was checked:

| Bug | What it made StrataKV look like | Real factor |
| --- | --- | --- |
| SQLite not using `F_FULLFSYNC` on macOS | ~2,000× faster on durable writes | **6×** of the gap was fake |
| Overwriting a key with the *same* bytes | competitive on overwrites | **395×** — SQLite was skipping the write |
| No page-cache warming before reads | whichever engine ran second won | **~3×** |
| Benchmarking on a loaded machine | the engine looked slow, inconsistently | **3.4×** swing on one workload |

And a fifth, in the opposite direction: the first write-amplification figure came out at
**1.02×**, which for a log-structured merge tree is not a good result but an impossible
one. It was measuring a workload too small to ever flush a memtable.

The lesson of this phase is the one that generalizes furthest: **a benchmark is a program
that produces a number whether or not it is measuring anything**, and a number that
flatters you is the one to distrust most.

## Why each bug produced a believable lie

### 1. `fsync` on macOS does not do what you think

`fsync()` on macOS tells the *drive* to accept the data, but not to write it to the
physical medium — the drive may hold it in its own volatile write cache and report
success. Apple's answer is `fcntl(fd, F_FULLFSYNC)`. StrataKV used it from P1
([src/env.cc](../../src/env.cc)); SQLite only uses it when you set `PRAGMA fullfsync=ON`.

So the first benchmark compared an engine that really flushed against one that did not,
and SQLite reported 305,227 "durable" writes per second — which should be read as a
hardware impossibility, not a result. No SSD does 305,227 physical flushes a second.

**Setting the pragma cost SQLite 6×** (704 → 120 ops/s), and that 6× was never StrataKV's
to claim.

The general rule: *two durability settings are comparable only if both engines are
promising the same thing.* The benchmark now reads all three pragmas back after setting
them and raises `AssertionError` if any did not take effect, because a pragma that
silently fails to apply is the same bug again, wearing a disguise.

### 2. Writing the same bytes twice is not an overwrite

The overwrite workload originally re-wrote each key with the value the preload had already
stored. A **B-tree** can notice the page is unchanged and skip the disk write entirely. An
LSM tree appends regardless, because it does not read before writing.

So the benchmark was measuring *no-op detection*, not overwrite cost. Measured directly:

```
overwrite with IDENTICAL value:   65,242 ops/s
overwrite with DIFFERENT value:      165 ops/s
                                  -> 395x
```

The fix is one line — write `make_value(value_bytes, operations + n)`, a value the preload
never stored — and it changes the result by a factor of nearly four hundred.

### 3. The page cache remembers the previous run

Reading a file the operating system already has in RAM is not a disk read. Whichever
engine ran second in a pair benefited from the first one's warm cache and looked about
**3× faster** for no reason of its own. The benchmark now touches every seventh key before
starting the timer, outside the timed section, for both engines equally.

### 4. A busy machine is indistinguishable from a slow engine

This one was caught by accident. The same workload measured **10,196 ops/s** and then
**34,685 ops/s** — a 3.4× swing — because the first run shared the machine with a
property-test sweep and a Spotlight re-index. Nothing in the output said so, and the
slower figure looked entirely reasonable.

`bench.py` now reads the load average before starting and **refuses to run** above half
the core count unless `--allow-busy-machine` is passed, and records the load average
before and after in the results file. A run that started quiet and ended loaded is a run
whose later rows cannot be compared with its earlier ones.

## Write amplification: when a good number is the bug

**Write amplification** is the bytes the engine really writes divided by the bytes of user
data it was asked to store. An LSM writes each record to the log, then to an L0 table,
then rewrites it in every compaction that touches it — so it should be clearly above 1,
and should *grow* as the tree deepens. That is the cost an LSM pays for fast writes, and
being able to state it is part of understanding the design.

The first measurement said **1.02×**.

That should be read as "the measurement is broken", not "the engine is remarkable". The
cause: the durable write workloads run far fewer operations than the buffered ones,
because every one of them waits for a real disk flush. At 2,000 operations of 1 KB that is
about 2 MB of user data against a 4 MB memtable — **the run never filled a memtable, so it
never flushed, never compacted, and wrote nothing but the log.** Write amplification of a
write-ahead log alone is just over 1.0.

The tell was the asymmetry: 1.02× on the durable rows and 2.59× on the buffered ones, for
the same engine doing the same kind of work. Identical engines, different amplification,
means the workloads differ — not the engine.

Two fixes, and the second is the real one:

1. Every row now records how many SSTables existed when it finished, and the output marks
   a run that never flushed as `(WAL only, never flushed)`. A figure that cannot be an LSM
   figure now says so on its own.
2. `measure_write_amplification` is a separate measurement sized to force repeated
   compaction: a 1 MB memtable, 100-byte values, keys written in **random order** so the
   L0 tables overlap and the compactor has real merging to do. Sequential insertion
   produces non-overlapping tables, which is the easy case.

The result behaves the way the theory says it should:

```
  25,000 keys    2.85 MB user     7.22 MB written   amp 2.54x   1 table
  50,000 keys    5.70 MB user    17.71 MB written   amp 3.11x   1 table
 100,000 keys   11.40 MB user    47.85 MB written   amp 4.20x   2 tables
 200,000 keys   22.80 MB user   153.45 MB written   amp 6.73x   6 tables
```

The number climbs because compaction's whole job is rewriting data it has already written.
The curve is the design trade-off made visible: writes are fast because they only append,
and the cost is paid later, repeatedly, by the compactor.

It climbs *this* steeply because StrataKV has only two levels. Every L0 to L1 compaction
rewrites the whole of L1, so touching L1 costs more as L1 grows. A production engine has
five or more levels precisely to bound this — each level is a fixed multiple of the one
above, so a record is rewritten a fixed number of times rather than a growing number. Being
able to explain *why* the slope looks like this is worth more than the number itself.

(A smaller run — 5,000 keys against a 1 MB memtable — reports 1.21× with **zero** tables:
the log and nothing else, which is the broken case described above.)

## How to read the results

Three honest conclusions from [NUMBERS.md](../NUMBERS.md), including the one that does not
favor this project:

- **The read-miss advantage widens with value size**: 1.96x at 100-byte values, 13.1x at
  1 KB. Read the *widening*, not either number alone. A bloom filter rejects a missing key
  from an in-memory bit array in constant time however large the rows are, while a
  B-tree's descent gets more expensive as fewer rows fit per page. Structure explains the
  trend; a single ratio would not have shown it.
- **Durable single-key writes are close**, which is expected: at one `fsync` per
  operation, both engines are measuring the disk, not themselves. When the hardware is the
  bottleneck the software mostly cannot matter — which is exactly why group commit (P4)
  mattered so much, since it changes *how many* flushes there are.
- **SQLite is ahead on 3 of the 20 comparisons**, all durable writes. That is a real
  result, it is in the README, and it should stay there. A benchmark that only reports the
  workloads you win is marketing.

There is a lesson in how this section was written, too. Its first draft said SQLite won at
*buffered* writes — a conclusion drawn from the run taken on a loaded machine, and simply
false once the machine was quiet. **A contaminated benchmark does not just produce wrong
numbers; it produces wrong explanations, and the explanations are what you remember.**

## Annotated real code

### Refusing to measure a busy machine

```python
def check_machine_is_quiet() -> float:
    load = load_average()
    cores = os.cpu_count() or 1
    if load == load and load > cores * 0.5:
        print("WARNING: load average is ...", file=sys.stderr, flush=True)
    return load
```

`load == load` looks like a tautology and is not: **NaN is the only value not equal to
itself**, so this is the idiomatic way to ask "did we get a real number?" on a platform
where `getloadavg` is unavailable. The check degrades to doing nothing rather than
crashing or, worse, comparing against NaN and silently always passing.

### Verifying a pragma actually applied

```python
actual = {
    name: connection.execute(f"PRAGMA {name}").fetchone()[0]
    for name in ("journal_mode", "synchronous", "fullfsync")
}
expected_synchronous = 2 if synchronous == "FULL" else 1
expected_fullfsync = 1 if synchronous == "FULL" else 0
if (
    actual["journal_mode"] != "wal"
    or actual["synchronous"] != expected_synchronous
    or actual["fullfsync"] != expected_fullfsync
):
    raise AssertionError(f"SQLite durability not as requested for {synchronous}: {actual}")
```

SQLite accepts a pragma it cannot honor without complaining — `PRAGMA journal_mode=WAL`
fails silently on an in-memory database, for instance. Note that the values come back as
integers (`synchronous` is `2` for `FULL`, `1` for `NORMAL`) rather than the words used to
set them, so the check has to know both spellings. Reading them back converts a silent
mismeasurement into a loud failure — the same instinct as asserting that a string
replacement matched before writing the file out, which this project learned the hard way
in P4.

## Charts, and why plotting is a separate program

[bench/plot.py](../../bench/plot.py) reads `results.json` and never runs a benchmark. That
separation is deliberate: every number in the README has to come from a recorded run with
a known command, machine, and date, so re-drawing a chart can never quietly change a
number. It is the same reason the results are written as JSON before anything is
formatted.

`matplotlib.use("Agg")` is called before `pyplot` is imported, because CI has no display
and the default interactive backend fails there rather than falling back.

## New C++ features

None. This phase added no C++ code, which is itself worth noticing: the engine was
finished before it was measured, and measuring it did not turn up anything that had to be
changed in it. The bugs this phase found were all in the measurement.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): `F_FULLFSYNC`, matched durability, load
average, B-tree, SQLite's WAL mode, a benchmarking note on page cache, and a sharpened
definition of write amplification. Read and space amplification were already there from
P3.

## Struggles

See the [P6 debrief](../DEBRIEF.md). The short version: four separate benchmark bugs, every
one of them flattering, and a write-amplification figure that was wrong precisely because
it looked good.

## Questions

Five questions written against this stage's real code are at the end of the
[P6 debrief](../DEBRIEF.md). They are worth answering out loud before reading the
answers — recognizing an answer and being able to say it are different skills.
