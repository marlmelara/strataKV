# Measured numbers

Every number that appears in the README or on a resume must appear here first, with the
exact command that produced it, the machine it ran on, and the date. A number that is not
in this file does not go on the resume; if a clause has no number, the clause gets deleted.

**No performance numbers exist yet.** P0 produced none, and none have been invented. The
first real measurements arrive in P4 (group commit throughput) and P6 (benchmarks against
SQLite).

## Machine profile

Referenced below as **M1** so each row does not have to repeat it.

| Field | Value |
| --- | --- |
| Label | M1 |
| CPU | Apple M2, 8 cores |
| Memory | 8 GB |
| Disk | Internal SSD (APFS) |
| OS | macOS 26.6.2 (arm64) |
| Compiler | Apple Clang 21.0.0 |
| CMake / Ninja | 4.4.4 / 1.13.2 |
| Python | 3.14.3 |

## Correctness results

These are pass/fail facts, not performance numbers, but they are what the project claims,
so they are recorded the same way.

| Result | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| C++ tests, debug preset | 26/26 passed | `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` | M1 | 2026-10-15 |
| C++ tests, release preset | 26/26 passed | `cmake --preset release && cmake --build --preset release && ctest --preset release` | M1 | 2026-10-15 |
| C++ tests, ASan + UBSan | 26/26 passed, 0 reports | `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` | M1 | 2026-10-15 |
| C++ tests, TSan | 26/26 passed, 0 reports | `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan` | M1 | 2026-10-15 |
| Python tests | 5/5 passed | `pip install '.[test]' && pytest` | M1 | 2026-10-15 |
| CRC32C known-answer, `"123456789"` | `0xE3069283` | `ctest --preset debug -R Crc32c.MatchesKnownAnswers` | M1 | 2026-10-15 |

## Performance numbers

Empty until P4. The table will use this shape:

| Metric | Value | Command | Machine | Date |
| --- | --- | --- | --- | --- |
| _(none yet)_ | | | | |

Planned rows, so it is obvious what is still missing:

- P4: durable writes/sec, 8 threads, group commit on vs off, and the ratio between them
- P6: sequential fill, random fill, random read (hit), random read (miss) — at 100 B and
  1 KB values, StrataKV vs SQLite at matched durability settings
- P6: write amplification (bytes written to disk / bytes written by the user)
