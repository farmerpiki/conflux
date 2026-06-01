#!/usr/bin/env python3
"""Keep package contract docs synchronized."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = "core`, `types`, `json`, and `file_io_sync"


def fail(message: str) -> None:
    print(f"check-package-docs: {message}", file=sys.stderr)
    raise SystemExit(1)


docs = {
    "docs/component-map.md": (ROOT / "docs/component-map.md").read_text(encoding="utf-8"),
    "docs/package-consumption.md": (ROOT / "docs/package-consumption.md").read_text(encoding="utf-8"),
    "docs/release-checklist.md": (ROOT / "docs/release-checklist.md").read_text(encoding="utf-8"),
}

for rel, text in docs.items():
    lowered = text.lower()
    for phrase in ["core", "types", "json", "file_io_sync"]:
        if phrase not in lowered:
            fail(f"{rel} does not mention {phrase}")
    if "runtime" not in lowered or "http" not in lowered:
        fail(f"{rel} does not document runtime/http package availability")

package = docs["docs/package-consumption.md"]
if "real-liburing install" not in package:
    fail("package-consumption.md must document real-liburing runtime/http installs")
if "DB-off install" not in package:
    fail("package-consumption.md must document DB-off install behavior")
for phrase in [
    "`MODULE_INTERFACE` is the primary",
    "Generated headers are release artifacts",
    "`HEADER_INTERFACE` exists for generated release artifacts",
]:
    if phrase not in package:
        fail(f"package-consumption.md must document modules-first artifact contract: {phrase}")

print("check-package-docs: ok")
