#!/usr/bin/env python3
"""Validate release-note skeleton sections."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NOTES = ROOT / "docs/releases/0.1.0-preview.md"


def fail(message: str) -> None:
    print(f"check-release-notes: {message}", file=sys.stderr)
    raise SystemExit(1)


text = NOTES.read_text(encoding="utf-8")
for heading in [
    "## Scope",
    "## Install And Consume",
    "## Supported Components",
    "## Evidence",
    "## Migration Notes",
    "## Known Limitations",
    "## Benchmark Claims Policy",
]:
    if heading not in text:
        fail(f"{NOTES.relative_to(ROOT)} missing heading {heading}")

for phrase in ["mock-liburing", "real-liburing", "core", "json", "file_io_sync", "runtime", "http"]:
    if phrase not in text:
        fail(f"release notes must mention {phrase}")

print("check-release-notes: ok")
