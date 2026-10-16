#!/usr/bin/env python3
"""Fail if the README quotes a figure that docs/NUMBERS.md does not record.

The project's standing rule is that every number in the README or on a resume
appears first in NUMBERS.md with the command, machine, and date that produced
it. A rule nobody checks is a rule that decays: it is far too easy to round a
figure in prose, or to quote a ratio that was computed in your head and never
written down anywhere a reader could verify.

This is deliberately crude. It looks for figures that would read as claims --
comma-grouped integers, decimal multipliers, percentages -- and insists each
one appears somewhere in NUMBERS.md. Derived figures (an inverse, a rounding)
count as recorded only if NUMBERS.md spells them out, which is the point: if a
reader would have to recompute it to check it, it belongs in the table.

    python tools/check_numbers.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Comma-grouped integers (12,345), decimal multipliers (4.74x), percentages.
# The multiplication sign is written as an escape rather than literally: this file
# exists to reconcile the two spellings, so having both characters sitting side by
# side in the source is exactly the ambiguity the linter rightly objects to.
TIMES = "\u00d7"
CLAIM = re.compile(rf"\b\d{{1,3}}(?:,\d{{3}})+\b|\b\d+\.\d+(?:{TIMES}|x)\b|\b\d+\.\d+%")

# Figures that are not claims about this engine: version numbers and the like.
# Keep this list short and justify each entry, or it becomes a way to silence
# the check rather than satisfy it.
EXEMPT: set[str] = set()


def prose_only(markdown: str) -> str:
    """Strip the parts of a document that are not claims to a reader.

    URLs and HTML attributes are full of digits that look like figures -- a
    badge pointing at "Python-3.11%2B" reads as "3.11%" to a regular
    expression. Those are addresses, not claims, so they are removed before
    anything is matched.
    """
    # Markdown link and image targets: the text stays, the target goes.
    markdown = re.sub(r"\]\([^)]*\)", "]", markdown)
    # Bare URLs, and every HTML tag with its attributes.
    markdown = re.sub(r"https?://\S+", "", markdown)
    markdown = re.sub(r"<[^>]+>", "", markdown)
    return markdown


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    readme = prose_only((root / "README.md").read_text())
    numbers = (root / "docs" / "NUMBERS.md").read_text()

    missing = []
    for token in sorted({m.group(0) for m in CLAIM.finditer(readme)}):
        if token in EXEMPT:
            continue
        # NUMBERS.md may spell a multiplier with "x" where the README uses TIMES.
        variants = {token, token.replace(TIMES, "x"), token.replace("x", TIMES)}
        if not any(variant in numbers for variant in variants):
            missing.append(token)

    if missing:
        print(f"{len(missing)} figure(s) in README.md are not recorded in docs/NUMBERS.md:\n")
        for token in missing:
            for line in readme.splitlines():
                if token in line:
                    print(f"  {token:<12} {line.strip()[:90]}")
                    break
        print(
            "\nAdd each one to docs/NUMBERS.md with the command that produced it, or, if it "
            "is derived\nfrom figures already there, write the derivation out so a reader "
            "does not have to recompute it."
        )
        return 1

    print("number check: every README figure is recorded in docs/NUMBERS.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
