"""Crash fuzzer: SIGKILL a writing process at random moments and demand its
acknowledged writes back.

This is the project's central claim, tested the only way it can really be
tested. A child process writes keys and prints each one after its write
returns. The parent kills it with SIGKILL -- no cleanup, no destructors, no
buffer flushing -- then reopens the database and insists that every key the
child printed is present with the right value.

Only *acknowledged* writes are checked. A write that was in flight when the
process died may or may not have survived, and both outcomes are correct: the
caller was never told it succeeded. Checking those would make the test fail on
behavior that is perfectly legal.

Some iterations also arm a crash point, so the kill lands inside a flush or a
compaction rather than wherever the scheduler happened to be. Random kills find
bugs probabilistically; named crash points hit the dangerous windows on purpose.
Using both is the strongest answer to "how do you know recovery works?"

    python tests/py/crash_fuzz.py --iterations 1000
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field

import stratakv

# The crash points the engine understands. An iteration either picks one or
# relies on the timing of the kill alone.
CRASH_POINTS = [
    None,
    "write_before_wal",
    "write_after_wal_sync",
    "flush_after_sst_write",
    "flush_before_wal_delete",
    "compaction_after_output",
    "compaction_before_manifest",
]


# These fire on the very first write, so nothing is ever acknowledged under
# them. They still have to leave a reopenable database, which is what the
# iteration checks; they simply cannot contribute verified writes.
FIRST_WRITE_CRASH_POINTS = frozenset({"write_before_wal", "write_after_wal_sync"})


@dataclass
class Results:
    iterations: int = 0
    acknowledged_total: int = 0
    killed_by_signal: int = 0
    exited_via_crash_point: int = 0
    empty_by_design: int = 0  # A crash point that fires on the first write.
    empty_unexpectedly: int = 0  # Killed before it got going: weak iteration.
    failures: list[str] = field(default_factory=list)
    crash_point_counts: dict[str, int] = field(default_factory=dict)


def run_iteration(
    directory: str, rng: random.Random, writer: str, results: Results, verbose: bool
) -> None:
    crash_point = rng.choice(CRASH_POINTS)
    label = crash_point or "(timing only)"
    results.crash_point_counts[label] = results.crash_point_counts.get(label, 0) + 1

    environment = dict(os.environ)
    if crash_point is not None:
        environment["STRATAKV_CRASH_AT"] = crash_point
    else:
        environment.pop("STRATAKV_CRASH_AT", None)

    child = subprocess.Popen(
        [sys.executable, writer, directory],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )

    # Let it run for a random slice of time, then kill it outright.
    #
    # The floor matters more than it looks. Starting Python, importing the
    # extension and opening the database costs well over a tenth of a second,
    # so a shorter sleep kills the child before it has acknowledged anything --
    # and an iteration that verifies nothing is an iteration that proved
    # nothing. The ceiling keeps a thousand iterations to a few minutes.
    time.sleep(rng.uniform(0.20, 0.60))
    if child.poll() is None:
        child.kill()  # SIGKILL: no cleanup, no destructors, no flush.

    stdout, stderr = child.communicate()
    returncode = child.returncode

    if returncode == -signal.SIGKILL:
        results.killed_by_signal += 1
    elif returncode == 1:
        # The armed crash point fired before the kill did.
        results.exited_via_crash_point += 1

    acknowledged: dict[str, str] = {}
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) == 2:
            acknowledged[parts[0]] = parts[1]

    results.acknowledged_total += len(acknowledged)
    if not acknowledged:
        if crash_point in FIRST_WRITE_CRASH_POINTS:
            results.empty_by_design += 1
        else:
            results.empty_unexpectedly += 1

    # Reopen and demand every acknowledged write.
    try:
        with stratakv.DB.open(directory) as db:
            for key, expected in acknowledged.items():
                actual = db.get(key.encode())
                if actual is None:
                    results.failures.append(f"[{label}] lost acknowledged write {key}")
                    return
                if actual != expected.encode():
                    results.failures.append(
                        f"[{label}] wrong value for {key}: {actual!r} != {expected.encode()!r}"
                    )
                    return
            # The database must also still be usable, not merely readable.
            db.put(b"after-recovery", b"ok")
            if db.get(b"after-recovery") != b"ok":
                results.failures.append(f"[{label}] database unusable after recovery")
    except stratakv.CorruptionError as error:
        results.failures.append(f"[{label}] corruption after crash: {error}")
    except Exception as error:
        results.failures.append(f"[{label}] reopen failed: {error!r}")

    if verbose:
        print(
            f"  iteration {results.iterations + 1}: {label}, "
            f"{len(acknowledged)} acknowledged, rc={returncode}",
            flush=True,
        )
    if stderr and "crash point" not in stderr:
        pass  # The child's stderr is noise unless something went wrong.


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=50)
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args(argv)

    seed = arguments.seed if arguments.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    writer = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fuzz_writer.py")
    if not os.path.exists(writer):
        print(f"cannot find {writer}", file=sys.stderr)
        return 2

    print(f"crash fuzzer: {arguments.iterations} iterations, seed {seed}", flush=True)
    results = Results()
    started = time.time()

    for _ in range(arguments.iterations):
        directory = tempfile.mkdtemp(prefix="stratakv_fuzz_")
        try:
            run_iteration(directory, rng, writer, results, arguments.verbose)
        finally:
            shutil.rmtree(directory, ignore_errors=True)
        results.iterations += 1
        if results.failures:
            break

    elapsed = time.time() - started
    print(
        f"\n{results.iterations} iterations in {elapsed:.1f}s\n"
        f"  acknowledged writes verified: {results.acknowledged_total}\n"
        f"  killed by SIGKILL:            {results.killed_by_signal}\n"
        f"  exited via a crash point:     {results.exited_via_crash_point}\n"
        f"  no writes, by design:         {results.empty_by_design}"
        " (crash point fires on the first write)\n"
        f"  no writes, killed too early:  {results.empty_unexpectedly}",
        flush=True,
    )
    print("  crash points exercised:")
    for label, count in sorted(results.crash_point_counts.items()):
        print(f"    {label:<28} {count}")

    if results.failures:
        print(f"\nFAILED ({len(results.failures)}):", file=sys.stderr)
        for failure in results.failures:
            print(f"  {failure}", file=sys.stderr)
        print(f"reproduce with --seed {seed}", file=sys.stderr)
        return 1

    if results.acknowledged_total == 0:
        print(
            "\nFAILED: not a single acknowledged write was verified, so this proved nothing",
            file=sys.stderr,
        )
        return 1

    # A run in which most iterations never got a write in is a run that mostly
    # tested process startup. Fail rather than report a reassuring number.
    weak = results.empty_unexpectedly
    if results.iterations >= 10 and weak > results.iterations // 2:
        print(
            f"\nFAILED: {weak} of {results.iterations} iterations were killed before "
            "acknowledging anything; raise the sleep floor",
            file=sys.stderr,
        )
        return 1

    print("\nzero acknowledged writes lost, zero corruption errors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
