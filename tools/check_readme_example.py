#!/usr/bin/env python3
"""Run the Python example in README.md and fail if it does not work.

This exists because the example was wrong. It passed ("put", key, value)
triples to write_batch, which actually takes (key, value) pairs with None
meaning delete, so the very first thing a reader would copy and paste raised
ValueError. Nothing caught it: prose is not executed, and the engine's own
tests use the real API rather than the documented one.

For a portfolio project that is worse than an ordinary bug. The README snippet
is the first code anyone runs, and it failing is the first impression.

    python tools/check_readme_example.py
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

FENCED_PYTHON = re.compile(r"```python\n(.*?)```", re.DOTALL)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    blocks = FENCED_PYTHON.findall((root / "README.md").read_text())
    if not blocks:
        print("no ```python block found in README.md", file=sys.stderr)
        return 1

    failures = 0
    for index, source in enumerate(blocks, start=1):
        with tempfile.TemporaryDirectory(prefix="readme_example_") as directory:
            # The example opens a fixed path for readability. Point it somewhere
            # disposable instead, so running the check cannot touch a real
            # database and two runs cannot collide.
            runnable = source.replace('"/tmp/mydb"', repr(str(Path(directory) / "mydb")))
            try:
                exec(compile(runnable, f"README.md:block{index}", "exec"), {"__name__": "__main__"})
            # Catching broadly on purpose: any exception at all means the
            # snippet a reader would paste does not work.
            except Exception as error:
                print(f"README python block {index} raised {type(error).__name__}: {error}")
                failures += 1

    if failures:
        print(f"\n{failures} README example(s) do not run.")
        return 1

    print(f"readme check: {len(blocks)} Python example(s) ran successfully")
    return 0


if __name__ == "__main__":
    sys.exit(main())
