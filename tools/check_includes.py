#!/usr/bin/env python3
"""Check that each source file includes the headers for what it actually uses.

This exists because of a real bug that reached CI. `src/env.cc` used
`std::atomic` without including `<atomic>`, and it compiled cleanly on macOS:
libc++ happens to pull `<atomic>` in through another header. GCC's libstdc++
does not, so every Linux job failed to build while every macOS job passed.

Relying on one standard library's transitive includes is relying on luck. This
catches it before a push rather than after, and it is cheap enough to run in the
lint job.

    python3 tools/check_includes.py

Exits non-zero and lists every file that uses a facility without including its
header. It is a heuristic -- it matches on names, not on a real parse -- so it
can miss things, but it does not produce false positives in practice.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Facility -> the patterns that mean the file uses it.
REQUIREMENTS: dict[str, list[str]] = {
    "algorithm": [
        r"std::sort\b",
        r"std::lower_bound\b",
        r"std::max\b",
        r"std::min\b",
        r"std::clamp\b",
        r"std::remove_if\b",
        r"std::any_of\b",
        r"std::reverse\b",
        r"std::fill_n\b",
        r"std::copy\b",
        r"std::is_sorted\b",
    ],
    "array": [r"std::array\b"],
    "atomic": [r"std::atomic\b", r"std::memory_order"],
    "chrono": [r"std::chrono\b"],
    "cmath": [r"std::lround\b"],
    "condition_variable": [r"std::condition_variable\b"],
    "cstddef": [r"\bsize_t\b"],
    "cstdint": [r"\buint\d+_t\b", r"\bint\d+_t\b"],
    "cstdlib": [r"std::getenv\b", r"std::_Exit\b", r"std::abort\b", r"std::atoi\b"],
    "cstring": [r"std::strerror\b", r"std::strcmp\b", r"std::strlen\b"],
    "deque": [r"std::deque\b"],
    "functional": [r"std::function\b"],
    "iterator": [r"std::next\b"],
    "map": [r"std::map\b"],
    "memory": [
        r"std::unique_ptr\b",
        r"std::shared_ptr\b",
        r"std::make_unique\b",
        r"std::make_shared\b",
    ],
    "mutex": [r"std::mutex\b", r"std::lock_guard\b", r"std::unique_lock\b"],
    "queue": [r"std::priority_queue\b"],
    "set": [r"std::set\b", r"std::multiset\b"],
    "span": [r"std::span\b"],
    "string": [r"std::string\b"],
    "string_view": [r"std::string_view\b"],
    "thread": [r"std::thread\b", r"std::this_thread\b"],
    "utility": [r"std::move\b", r"std::pair\b", r"std::make_pair\b"],
    "vector": [r"std::vector\b"],
}


def sources() -> list[Path]:
    paths = list(Path("src").glob("*.cc")) + list(Path("src").glob("*.h"))
    paths += list(Path("include").rglob("*.h"))
    paths += list(Path("python").glob("*.cc"))
    return sorted(paths)


def check(path: Path) -> list[str]:
    text = path.read_text()
    # Ignore the include lines themselves, so `#include <memory>` does not count
    # as a use of std::unique_ptr.
    body = re.sub(r"^#include.*$", "", text, flags=re.MULTILINE)
    included = set(re.findall(r"#include <([^>]+)>", text))

    problems = []
    for header, patterns in REQUIREMENTS.items():
        if header in included:
            continue
        for pattern in patterns:
            match = re.search(pattern, body)
            if match:
                problems.append(f"{path}: uses {match.group(0)} without #include <{header}>")
                break
    return problems


def main() -> int:
    problems = [problem for path in sources() for problem in check(path)]
    for problem in problems:
        print(problem)
    if problems:
        print(f"\n{len(problems)} missing include(s).")
        print("A header that only compiles because another one pulled it in is luck,")
        print("and it breaks on a different standard library. Include what you use.")
        return 1
    print(f"include check: {len(sources())} files, all clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
