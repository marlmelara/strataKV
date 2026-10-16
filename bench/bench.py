"""Benchmark StrataKV against SQLite at matched durability settings.

Every number this prints is intended to end up in docs/NUMBERS.md with the
command that produced it, so two things matter more than the numbers
themselves:

**Matched durability.** Comparing a database that really flushes to one that
does not is meaningless, and it is the single easiest way to produce a
flattering benchmark by accident. StrataKV's `sync=True` is compared against
SQLite's `synchronous=FULL`, and `sync=False` against `synchronous=NORMAL`.
SQLite runs in WAL mode throughout, which is its fastest durable configuration.

**Identical work.** Both engines get the same keys, the same values, the same
operation counts, in the same order, from the same seed.

    python bench/bench.py --output bench/results/results.json

Results are written as JSON so plot.py can draw them without re-running
anything.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field

import stratakv
from stratakv import _stratakv as core


@dataclass
class Result:
    engine: str
    durability: str
    workload: str
    value_bytes: int
    operations: int
    seconds: float
    ops_per_second: float
    bytes_written: int | None = None
    write_amplification: float | None = None
    # Number of SSTables on disk when the run ended. Zero means the run never
    # filled a memtable, so nothing was ever flushed or compacted and the
    # write amplification above is the write-ahead log alone. That is not an
    # LSM number, and reading it as one is how you end up publishing a
    # flattering 1.02x. See measure_write_amplification for the real figure.
    sst_files: int | None = None


@dataclass
class Suite:
    machine: dict[str, str] = field(default_factory=dict)
    durability: dict[str, str] = field(
        default_factory=lambda: {
            "sync": (
                "StrataKV kSyncEveryWrite (fcntl F_FULLFSYNC) vs SQLite WAL, "
                "synchronous=FULL, fullfsync=ON"
            ),
            "nosync": "StrataKV kNoSync vs SQLite WAL, synchronous=NORMAL",
            "note": (
                "fullfsync=ON matters on macOS: without it SQLite calls fsync(), which "
                "does not flush the drive's own write cache, so the two engines would be "
                "promising different things. Measured here, turning it on costs SQLite 6x."
            ),
            "overwrite_note": (
                "The overwrite workload writes a value different from the one already "
                "stored. Rewriting identical bytes lets a B-tree skip the write entirely "
                "-- measured at 395x faster for SQLite -- which would measure no-op "
                "detection rather than overwrite cost."
            ),
        }
    )
    results: list[dict] = field(default_factory=list)
    write_amplification: list[dict] = field(default_factory=list)


def make_key(index: int) -> bytes:
    return b"key-%010d" % index


def make_value(size: int, index: int) -> bytes:
    # Deterministic and incompressible-ish, so neither engine benefits from a
    # pattern the other does not see.
    seed = b"%010d" % index
    return (seed * ((size // len(seed)) + 1))[:size]


# --- StrataKV ---


def bench_stratakv(
    workload: str, value_bytes: int, operations: int, sync: bool, seed: int
) -> Result:
    directory = tempfile.mkdtemp(prefix="bench_stratakv_")
    try:
        path = os.path.join(directory, "db")
        rng = random.Random(seed)

        with stratakv.DB.open(path, sync=sync) as db:
            if workload in ("random_read_hit", "random_read_miss", "overwrite"):
                # Preload, outside the timed section.
                for i in range(operations):
                    db.put(make_key(i), make_value(value_bytes, i))

            if workload in ("random_read_hit", "random_read_miss"):
                # Warm the cache before timing. Measured cold-then-warm, the
                # second run of a pair looked three times faster purely from
                # page-cache residue.
                for i in range(0, operations, 7):
                    db.get(make_key(i))

            core.reset_bytes_written()
            started = time.perf_counter()

            if workload == "sequential_fill":
                for i in range(operations):
                    db.put(make_key(i), make_value(value_bytes, i))
            elif workload == "random_fill":
                order = list(range(operations))
                rng.shuffle(order)
                for i in order:
                    db.put(make_key(i), make_value(value_bytes, i))
            elif workload == "overwrite":
                for n in range(operations):
                    i = rng.randrange(operations)
                    # A DIFFERENT value than the preload stored. Rewriting the
                    # identical bytes lets a B-tree notice the page is
                    # unchanged and skip the write entirely -- measured at 395x
                    # faster for SQLite -- while an LSM appends regardless. That
                    # would not be an overwrite benchmark, it would be a
                    # no-op-detection benchmark.
                    db.put(make_key(i), make_value(value_bytes, operations + n))
            elif workload == "random_read_hit":
                for _ in range(operations):
                    i = rng.randrange(operations)
                    if db.get(make_key(i)) is None:
                        raise AssertionError(f"missing key {i}")
            elif workload == "random_read_miss":
                for i in range(operations):
                    # Keys that were never written, so the bloom filters decide.
                    if db.get(b"absent-%010d" % i) is not None:
                        raise AssertionError("found a key that was never written")
            else:
                raise ValueError(workload)

            elapsed = time.perf_counter() - started
            written = core.bytes_written()

        tables = len([n for n in os.listdir(path) if n.endswith(".sst")])
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    user_bytes = operations * (len(make_key(0)) + value_bytes)
    is_write = workload in ("sequential_fill", "random_fill", "overwrite")
    return Result(
        engine="stratakv",
        durability="sync" if sync else "nosync",
        workload=workload,
        value_bytes=value_bytes,
        operations=operations,
        seconds=elapsed,
        ops_per_second=operations / elapsed,
        bytes_written=written if is_write else None,
        write_amplification=(written / user_bytes) if is_write and user_bytes else None,
        sst_files=tables if is_write else None,
    )


# --- SQLite ---


def open_sqlite(path: str, synchronous: str) -> sqlite3.Connection:
    connection = sqlite3.connect(path, isolation_level=None)  # autocommit
    # WAL is SQLite's fastest durable mode, so this is its best showing rather
    # than a straw man.
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute(f"PRAGMA synchronous={synchronous}")

    if synchronous == "FULL":
        # Without this the comparison is not matched durability, it is a
        # different guarantee with the same name.
        #
        # On macOS, plain fsync() returns once the data reaches the drive and
        # does NOT ask the drive to empty its own volatile write cache, so a
        # power cut can still lose it. SQLite calls fsync() by default;
        # StrataKV calls fcntl(F_FULLFSYNC), which does force that flush and
        # costs roughly 2.7 ms here against a few microseconds.
        #
        # Measured without this pragma, SQLite reported 305,000 synced
        # writes/sec -- about 3 microseconds each, which is proof on its face
        # that nothing reached the platter. Turning it on makes both engines
        # promise the same thing, which is the only basis on which the numbers
        # mean anything.
        connection.execute("PRAGMA fullfsync=ON")

    connection.execute("CREATE TABLE IF NOT EXISTS kv (k BLOB PRIMARY KEY, v BLOB) WITHOUT ROWID")

    # Read the settings back and insist they took effect.
    #
    # A pragma that silently does not apply produces a number that looks like a
    # result and is not one -- which is how an unfair benchmark gets published
    # by accident rather than by intent. Checking costs three queries.
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

    return connection


def bench_sqlite(
    workload: str, value_bytes: int, operations: int, synchronous: str, seed: int
) -> Result:
    directory = tempfile.mkdtemp(prefix="bench_sqlite_")
    try:
        path = os.path.join(directory, "db.sqlite")
        rng = random.Random(seed)
        connection = open_sqlite(path, synchronous)
        try:
            if workload in ("random_read_hit", "random_read_miss", "overwrite"):
                for i in range(operations):
                    connection.execute(
                        "INSERT OR REPLACE INTO kv VALUES (?, ?)",
                        (make_key(i), make_value(value_bytes, i)),
                    )

            if workload in ("random_read_hit", "random_read_miss"):
                for i in range(0, operations, 7):
                    connection.execute("SELECT v FROM kv WHERE k = ?", (make_key(i),)).fetchone()

            started = time.perf_counter()

            if workload == "sequential_fill":
                for i in range(operations):
                    connection.execute(
                        "INSERT OR REPLACE INTO kv VALUES (?, ?)",
                        (make_key(i), make_value(value_bytes, i)),
                    )
            elif workload == "random_fill":
                order = list(range(operations))
                rng.shuffle(order)
                for i in order:
                    connection.execute(
                        "INSERT OR REPLACE INTO kv VALUES (?, ?)",
                        (make_key(i), make_value(value_bytes, i)),
                    )
            elif workload == "overwrite":
                for n in range(operations):
                    i = rng.randrange(operations)
                    # Must match the StrataKV side exactly: a genuinely new
                    # value, so neither engine can skip the write.
                    connection.execute(
                        "INSERT OR REPLACE INTO kv VALUES (?, ?)",
                        (make_key(i), make_value(value_bytes, operations + n)),
                    )
            elif workload == "random_read_hit":
                for _ in range(operations):
                    i = rng.randrange(operations)
                    row = connection.execute(
                        "SELECT v FROM kv WHERE k = ?", (make_key(i),)
                    ).fetchone()
                    if row is None:
                        raise AssertionError(f"missing key {i}")
            elif workload == "random_read_miss":
                for i in range(operations):
                    row = connection.execute(
                        "SELECT v FROM kv WHERE k = ?", (b"absent-%010d" % i,)
                    ).fetchone()
                    if row is not None:
                        raise AssertionError("found a key that was never written")
            else:
                raise ValueError(workload)

            elapsed = time.perf_counter() - started
        finally:
            connection.close()
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    return Result(
        engine="sqlite",
        durability="sync" if synchronous == "FULL" else "nosync",
        workload=workload,
        value_bytes=value_bytes,
        operations=operations,
        seconds=elapsed,
        ops_per_second=operations / elapsed,
    )


def machine_description() -> dict[str, str]:
    def run(command: list[str]) -> str:
        try:
            return subprocess.check_output(command, text=True).strip()
        except Exception:
            return "unknown"

    cpu = "unknown"
    if sys.platform == "darwin":
        cpu = run(["sysctl", "-n", "machdep.cpu.brand_string"])
    return {
        "cpu": cpu,
        "cores": str(os.cpu_count()),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "sqlite": sqlite3.sqlite_version,
        "stratakv": stratakv.version(),
    }


def load_average() -> float:
    try:
        return os.getloadavg()[0]
    except (OSError, AttributeError):  # not available on every platform
        return float("nan")


def check_machine_is_quiet() -> float:
    """Refuse to benchmark a machine that is already busy.

    This exists because of a real mistake: a run was started while a
    property-test sweep and a Spotlight re-index were using the machine, and
    the same workload measured 10,196 ops/s then and 34,685 ops/s afterwards.
    Nothing in the output said why, and the slower figure looked perfectly
    plausible. A benchmark that cannot tell a busy machine from a slow engine
    is not measuring the engine.
    """
    load = load_average()
    cores = os.cpu_count() or 1
    # NaN is the only value not equal to itself, and it is how an unavailable
    # getloadavg() reports itself here. Checking that first means the gate
    # degrades to doing nothing on a platform without load averages, rather
    # than comparing against NaN -- which is always false, so the check would
    # silently always pass.
    if load == load and load > cores * 0.5:
        print(
            f"WARNING: load average is {load:.2f} on {cores} cores. Something else is\n"
            f"         using this machine, and these timings will understate the\n"
            f"         engine. Wait for it to settle, or pass --allow-busy-machine\n"
            f"         to record the numbers anyway.",
            file=sys.stderr,
            flush=True,
        )
    return load


WORKLOADS = [
    "sequential_fill",
    "random_fill",
    "overwrite",
    "random_read_hit",
    "random_read_miss",
]


# --- Write amplification ---
#
# Write amplification is the bytes the storage engine really hands to the
# operating system divided by the bytes of user data it was asked to store. An
# LSM writes each record to the log, then to an L0 table, then rewrites it in
# every compaction that touches it, so the figure should be meaningfully above
# 1 and should grow as the tree deepens.
#
# This needs its own measurement rather than a column on the throughput table,
# because the throughput runs are sized for time rather than for depth. A run
# that writes less than one memtable never flushes, so its "write
# amplification" is the write-ahead log alone and comes out just over 1.0 --
# which looks like an excellent result and is actually the absence of one.


@dataclass
class WriteAmplification:
    keys: int
    value_bytes: int
    memtable_bytes: int
    user_bytes: int
    bytes_written: int
    write_amplification: float
    sst_files: int
    disk_bytes: int
    seconds: float


def measure_write_amplification(
    keys: int, value_bytes: int, memtable_bytes: int, seed: int
) -> WriteAmplification:
    directory = tempfile.mkdtemp(prefix="bench_writeamp_")
    try:
        path = os.path.join(directory, "db")
        # Shuffled, because keys inserted in order produce L0 tables whose
        # ranges do not overlap, which is the easiest case for a compactor.
        # Random order forces real merging and is the honest figure.
        order = list(range(keys))
        random.Random(seed).shuffle(order)

        # sync=False: the sync policy changes how often the engine waits for
        # the disk, not how many bytes it writes, so the cheaper setting
        # measures the same amplification in a fraction of the time.
        with stratakv.DB.open(path, sync=False, memtable_bytes=memtable_bytes) as db:
            core.reset_bytes_written()
            started = time.perf_counter()
            for i in order:
                db.put(make_key(i), make_value(value_bytes, i))
            elapsed = time.perf_counter() - started
            written = core.bytes_written()

        names = os.listdir(path)
        disk_bytes = sum(os.path.getsize(os.path.join(path, n)) for n in names)
        tables = len([n for n in names if n.endswith(".sst")])
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    user_bytes = keys * (len(make_key(0)) + value_bytes)
    return WriteAmplification(
        keys=keys,
        value_bytes=value_bytes,
        memtable_bytes=memtable_bytes,
        user_bytes=user_bytes,
        bytes_written=written,
        write_amplification=written / user_bytes,
        sst_files=tables,
        disk_bytes=disk_bytes,
        seconds=elapsed,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operations", type=int, default=20000)
    parser.add_argument(
        "--sync-operations",
        type=int,
        default=2000,
        help="fewer, because every one of them really reaches the disk",
    )
    parser.add_argument("--value-sizes", type=int, nargs="+", default=[100, 1024])
    parser.add_argument("--seed", type=int, default=20261016)
    parser.add_argument(
        "--write-amp-keys",
        type=int,
        nargs="+",
        default=[25_000, 50_000, 100_000, 200_000],
        help="key counts for the write-amplification curve",
    )
    parser.add_argument(
        "--write-amp-memtable-bytes",
        type=int,
        default=1 << 20,
        help="small enough that the key counts above force repeated compaction",
    )
    parser.add_argument(
        "--allow-busy-machine",
        action="store_true",
        help="record timings even though another process is using the machine",
    )
    parser.add_argument("--output", default="bench/results/results.json")
    arguments = parser.parse_args(argv)

    suite = Suite(machine=machine_description())
    cores = os.cpu_count() or 1
    load_before = check_machine_is_quiet()
    if load_before > cores * 0.5 and not arguments.allow_busy_machine:
        print("refusing to benchmark a busy machine; see the warning above", file=sys.stderr)
        return 2
    suite.machine["load_average_before"] = f"{load_before:.2f}"
    print("machine:", json.dumps(suite.machine, indent=2), flush=True)

    for value_bytes in arguments.value_sizes:
        for workload in WORKLOADS:
            for durable in (True, False):
                # A synced run does one real disk flush per operation, so it
                # needs far fewer operations to take a comparable time.
                is_write = workload in ("sequential_fill", "random_fill", "overwrite")
                count = (
                    arguments.sync_operations if (durable and is_write) else arguments.operations
                )

                for result in (
                    bench_stratakv(workload, value_bytes, count, durable, arguments.seed),
                    bench_sqlite(
                        workload,
                        value_bytes,
                        count,
                        "FULL" if durable else "NORMAL",
                        arguments.seed,
                    ),
                ):
                    suite.results.append(asdict(result))
                    extra = ""
                    if result.write_amplification is not None:
                        extra = f"  write amp {result.write_amplification:.2f}x"
                        if not result.sst_files:
                            extra += " (WAL only, never flushed)"
                    print(
                        f"{result.engine:<9} {result.durability:<7} {result.workload:<17}"
                        f" {result.value_bytes:>5}B  {result.ops_per_second:>10,.0f} ops/s{extra}",
                        flush=True,
                    )

    print(
        "\nwrite amplification (random order, 100B values, memtable "
        f"{arguments.write_amp_memtable_bytes:,}B):",
        flush=True,
    )
    for keys in arguments.write_amp_keys:
        amplification = measure_write_amplification(
            keys, 100, arguments.write_amp_memtable_bytes, arguments.seed
        )
        suite.write_amplification.append(asdict(amplification))
        print(
            f"  {amplification.keys:>8,} keys  "
            f"{amplification.user_bytes / 1e6:>7.1f} MB user  "
            f"{amplification.bytes_written / 1e6:>7.1f} MB written  "
            f"write amp {amplification.write_amplification:>5.2f}x  "
            f"{amplification.sst_files:>3} tables  "
            f"{amplification.disk_bytes / 1e6:>6.1f} MB on disk",
            flush=True,
        )

    # Recorded after as well: a run that started quiet and ended loaded is a
    # run whose later rows cannot be compared with its earlier ones.
    suite.machine["load_average_after"] = f"{load_average():.2f}"

    os.makedirs(os.path.dirname(arguments.output), exist_ok=True)
    with open(arguments.output, "w") as handle:
        json.dump(asdict(suite), handle, indent=2)
    print(f"\nwrote {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
