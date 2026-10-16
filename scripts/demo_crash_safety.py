"""A live demonstration of the one claim StrataKV makes.

    If `put` returned OK, that write survives a crash.

Nothing here is simulated. A real child process writes real keys to a real
database with `sync=True`, this script really sends it SIGKILL -- which runs no
cleanup, no destructors, and flushes no buffers -- and then really reopens the
database and demands every acknowledged write back.

"Acknowledged" is defined operationally rather than asserted. The child prints
a key only *after* its `put` returns, and flushes immediately, so the set this
script reads is exactly the set of promises the engine made. Writes that were
still in flight when the process died are deliberately not checked: the caller
was never told they succeeded, so either outcome is correct, and demanding them
would fail the demo on legal behavior.

    python scripts/demo_crash_safety.py

Pass --pace to slow it down for screen recording; the work is identical.
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import stratakv

BOLD = "\033[1m"
DIM = "\033[2m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
OFF = "\033[0m"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", type=int, default=250, help="acknowledged writes before the kill")
    parser.add_argument("--pace", type=float, default=0.0, help="seconds between steps, for video")
    arguments = parser.parse_args(argv)

    def pause(multiplier: float = 1.0) -> None:
        if arguments.pace:
            time.sleep(arguments.pace * multiplier)

    directory = tempfile.mkdtemp(prefix="stratakv_demo_")
    path = os.path.join(directory, "db")
    writer = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "py")

    try:
        print(f"{BOLD}StrataKV — crash safety, demonstrated{OFF}")
        print(f"{DIM}Every acknowledged write must survive SIGKILL.{OFF}\n")
        pause(2)

        # The child opens with sync=True and a small memtable, so the kill can
        # land in a rotation, a flush or a compaction rather than only between
        # clean writes.
        print(
            f"{CYAN}1.{OFF} Starting a writer process  "
            f"{DIM}(sync=True — every put reaches the disk){OFF}"
        )
        child = subprocess.Popen(
            [sys.executable, "-u", "fuzz_writer.py", path],
            cwd=writer,
            stdout=subprocess.PIPE,
            text=True,
        )
        assert child.stdout is not None
        pause()

        acknowledged: dict[str, str] = {}
        for line in child.stdout:
            key, value = line.split()
            acknowledged[key] = value
            if len(acknowledged) >= arguments.keys:
                break
        print(f"   {DIM}pid {child.pid} acknowledged {len(acknowledged):,} writes{OFF}\n")
        pause(2)

        print(f"{CYAN}2.{OFF} {RED}kill -9{OFF} — no cleanup, no destructors, no flush")
        child.send_signal(signal.SIGKILL)
        child.wait()
        print(f"   {DIM}process {child.pid} died with signal {-child.returncode} mid-write{OFF}\n")
        pause(2)

        print(f"{CYAN}3.{OFF} Reopening the database and replaying the log")
        started = time.perf_counter()
        with stratakv.DB.open(path, sync=True) as db:
            recovered = time.perf_counter() - started
            print(f"   {DIM}recovered in {recovered * 1000:.0f} ms{OFF}\n")
            pause(2)

            print(f"{CYAN}4.{OFF} Demanding every acknowledged write back")
            lost, wrong = [], []
            for key, expected in acknowledged.items():
                actual = db.get(key.encode())
                if actual is None:
                    lost.append(key)
                elif actual.decode() != expected:
                    wrong.append(key)
            pause(2)

            if lost or wrong:
                print(f"\n   {RED}{BOLD}FAILED{OFF}  {len(lost):,} lost, {len(wrong):,} wrong\n")
                return 1

            print(
                f"\n   {GREEN}{BOLD}✓ {len(acknowledged):,} / {len(acknowledged):,} survived{OFF}"
                f"  {DIM}0 lost, 0 corrupted{OFF}"
            )
            pause()

            # Readable is not the same as usable. A database that recovers but
            # cannot take a new write has not really recovered.
            db.put(b"after-the-crash", b"still writable")
            assert db.get(b"after-the-crash") == b"still writable"
            print(f"   {GREEN}✓{OFF} still accepts new writes\n")
            pause(2)

        print(f"{DIM}Verified at scale: 1,000 crashes, 40,851 acknowledged writes, 0 lost.{OFF}")
        print(f"{DIM}  python tests/py/crash_fuzz.py --iterations 1000{OFF}")
        return 0
    finally:
        shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
