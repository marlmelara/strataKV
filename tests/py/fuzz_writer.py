"""Child process for the crash fuzzer: writes keys until it is killed.

Each key is printed to stdout *after* its write returns, and stdout is flushed
immediately, so whatever the parent manages to read is exactly the set of
acknowledged writes. Nothing else may be printed to stdout.

Two details that make the fuzzer's verdict trustworthy:

* Output is flushed after every line. A SIGKILL runs no cleanup and flushes no
  buffers, so a buffered line would be lost precisely in the case that matters
  -- and the parent would then not demand a write that actually was promised,
  quietly weakening the check.
* Each key is written exactly once per run. If a key were written twice, a
  later *unacknowledged* write of it could legitimately be on disk after the
  crash, and the parent comparing against the earlier acknowledged value would
  report a failure that is not one.

Usage:
    python -m fuzz_writer <db_path> [--crash-at NAME]
"""

from __future__ import annotations

import argparse
import os
import sys

import stratakv


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write keys until killed.")
    parser.add_argument("path")
    parser.add_argument(
        "--memtable-bytes",
        type=int,
        default=8192,
        help="small, so rotations, flushes and compactions all happen under the crash",
    )
    arguments = parser.parse_args(argv)

    # sync=True is the whole point: an acknowledged write must have reached the
    # disk, which is exactly what the parent will insist on after the kill.
    with stratakv.DB.open(arguments.path, sync=True, memtable_bytes=arguments.memtable_bytes) as db:
        index = 0
        while True:
            key = f"key-{index:08d}"
            value = f"value-{index:08d}"
            db.put(key.encode(), value.encode())

            # Printed only after put returned. This line IS the promise.
            sys.stdout.write(f"{key} {value}\n")
            sys.stdout.flush()
            index += 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except BrokenPipeError:
        # The parent stopped reading; nothing is wrong with the database.
        os._exit(0)
