#!/usr/bin/env python3
"""A bit-by-bit reference implementation of CRC-32C (Castagnoli).

This exists to cross-check ``src/crc32c.cc``. It is deliberately table-free, so it
shares no code and no logic with the C++ implementation beyond the polynomial
itself -- two independent implementations agreeing is much stronger evidence than
one implementation agreeing with itself.

Note that Python's ``zlib.crc32`` is a *different* polynomial (CRC-32, not CRC-32C)
and will not match. That is exactly why this file is needed.

Run it to print the expected values asserted by
``tests/cpp/test_crc32c.cc::Crc32c.MatchesKnownAnswers``:

    python3 tools/crc32c_reference.py
"""

from __future__ import annotations

# The Castagnoli polynomial, bit-reversed -- the same constant as kPolynomial in
# src/crc32c.cc.
POLYNOMIAL = 0x82F63B78


def crc32c(data: bytes, crc: int = 0) -> int:
    """Return the CRC-32C of ``data``, optionally continuing from ``crc``.

    Processes one bit at a time rather than using a lookup table, which is slow
    and completely independent of the table-driven C++ version.
    """
    c = crc ^ 0xFFFFFFFF
    for byte in data:
        c ^= byte
        for _ in range(8):
            c = (c >> 1) ^ (POLYNOMIAL if (c & 1) else 0)
    return c ^ 0xFFFFFFFF


# The vectors asserted in tests/cpp/test_crc32c.cc. "123456789" is the one the
# build spec names as the P0 acceptance criterion; the 32-byte patterns are the
# standard iSCSI (RFC 3720) vectors.
VECTORS: dict[str, bytes] = {
    "empty": b"",
    '"a"': b"a",
    '"abc"': b"abc",
    '"123456789"': b"123456789",
    '"hello world"': b"hello world",
    "32 x 0x00": bytes(32),
    "32 x 0xff": b"\xff" * 32,
    "0x00..0x1f": bytes(range(32)),
    "0x1f..0x00": bytes(range(31, -1, -1)),
}


def main() -> None:
    for name, value in VECTORS.items():
        print(f"{name:<16} 0x{crc32c(value):08X}")

    # The property WalWriter::AddRecord depends on: checksumming a record's type
    # byte and then continuing over the payload must equal checksumming the two
    # concatenated.
    head, tail = b"\x01", b"payload bytes"
    incremental = crc32c(tail, crc32c(head))
    combined = crc32c(head + tail)
    status = "MATCH" if incremental == combined else "MISMATCH"
    print(f"\nExtend property: 0x{incremental:08X} vs 0x{combined:08X}  {status}")


if __name__ == "__main__":
    main()
