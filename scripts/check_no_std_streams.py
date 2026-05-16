#!/usr/bin/env python3
"""Fail when reusable src/ code regresses to standard stream APIs."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("standard stream header", re.compile(r"#\s*include\s*<(?:iostream|fstream|sstream|iosfwd|syncstream)>")),
    ("standard stream type/object", re.compile(r"\bstd::(?:if?stream|ofstream|fstream|stringstream|istringstream|ostringstream|istreambuf_iterator|[io]stream|cin|cout|cerr|clog)\b")),
    ("unqualified standard stream vocabulary", re.compile(r"\b(?:ifstream|ofstream|fstream|stringstream|istringstream|ostringstream|istreambuf_iterator|cin|cout|cerr|clog)\b")),
    ("types std::println export", re.compile(r"\bexport\s+using\s+std::println\s*;")),
    ("types std::cerr export", re.compile(r"\bexport\s+using\s+std::cerr\s*;")),
)

WHOLE_FILE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "standard print to stderr",
        re.compile(r"\bstd::(?:print|println)\s*\(\s*(?:stderr|std::cerr)\b", re.DOTALL),
    ),
)


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def main() -> int:
    failures: list[str] = []
    for path in sorted(SRC.rglob("*.cxx")):
        text = path.read_text(errors="ignore")
        rel = path.relative_to(ROOT)
        for line_no, line in enumerate(text.splitlines(), 1):
            for label, pattern in PATTERNS:
                if pattern.search(line):
                    failures.append(f"{rel}:{line_no}: {label}: {line.strip()}")
        for label, pattern in WHOLE_FILE_PATTERNS:
            for match in pattern.finditer(text):
                line_no = line_number(text, match.start())
                line = text.splitlines()[line_no - 1].strip()
                failures.append(f"{rel}:{line_no}: {label}: {line}")
    if failures:
        print("no-std-streams guard failed:", file=sys.stderr)
        for item in failures:
            print(item, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
