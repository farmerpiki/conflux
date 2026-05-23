#!/usr/bin/env python3
"""Fail when an empty catch-all in src/ lacks an explicit rationale comment."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

EMPTY_CATCH_RE = re.compile(r"catch\s*\(\s*\.\.\.\s*\)\s*\{\s*\}")
RATIONALE_RE = re.compile(r"//\s*NOLINT\(bugprone-empty-catch\):\s*\S")


def main() -> int:
    failures: list[str] = []
    for path in sorted(SRC.rglob("*.cxx")):
        text = path.read_text(errors="ignore")
        for line_no, line in enumerate(text.splitlines(), 1):
            if EMPTY_CATCH_RE.search(line) and not RATIONALE_RE.search(line):
                rel = path.relative_to(ROOT)
                failures.append(f"{rel}:{line_no}: empty catch-all needs a NOLINT rationale: {line.strip()}")
    if failures:
        print("empty-catch rationale guard failed:", file=sys.stderr)
        for item in failures:
            print(item, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
