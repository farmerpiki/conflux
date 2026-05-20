#!/usr/bin/env python3
"""Reject stale public spellings in first-contact docs/examples."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATHS = [
    ROOT / "README.md",
    ROOT / "docs",
    ROOT / "examples" / "quickstart",
]
SKIP = {
    ROOT / "docs" / "archive",
    ROOT / "docs" / "README.md",
    ROOT / "docs" / "planning-policy.md",
    ROOT / "docs" / "naming-audit.md",
    ROOT / "docs" / "io_uring_direct_file_flow_design.md",
}

PATTERN = re.compile(
    r"\b(S|SV|SP|UP|Opt|Fn|Tup|RE|EC|SZ)\b"
    r"|(?<![A-Za-z0-9_])(?:send_async|proxy_async|dispatch_async|block_on_socket_task|write_all_fd|read_all_fd)(?![A-Za-z0-9_])"
    r"|Config::benchmark\(\)"
)


def files() -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for path in PATHS:
        if path.is_file():
            found.append(path)
        else:
            found.extend(p for p in path.rglob("*") if p.is_file())
    return sorted(found)


def skipped(path: pathlib.Path) -> bool:
    return any(path == item or item in path.parents for item in SKIP)


def main() -> int:
    failures: list[str] = []
    for path in files():
        if skipped(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for line_no, line in enumerate(text.splitlines(), start=1):
            if "cmake -S" in line:
                continue
            if ("todo/" in line or "proposals/" in line) and "benchmarks/README.md" not in line:
                rel = path.relative_to(ROOT)
                failures.append(f"{rel}:{line_no}: first-contact docs must not route users through planning files")
            if PATTERN.search(line):
                rel = path.relative_to(ROOT)
                failures.append(f"{rel}:{line_no}: {line.strip()}")

    if failures:
        print("first-contact public dialect check failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
