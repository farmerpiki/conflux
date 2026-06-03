#!/usr/bin/env python3
"""Validate release-note skeleton sections."""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NOTES = ROOT / "docs/releases/0.1.0-preview.md"


def fail(message: str) -> None:
    print(f"check-release-notes: {message}", file=sys.stderr)
    raise SystemExit(1)


text = NOTES.read_text(encoding="utf-8")
release_skus = json.loads((ROOT / "docs/release-skus.json").read_text(encoding="utf-8"))
if not isinstance(release_skus, dict):
    fail("docs/release-skus.json must be a JSON object")

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

for phrase in ["real-liburing", "core", "json", "file_io_sync", "runtime", "http"]:
    if phrase not in text:
        fail(f"release notes must mention {phrase}")

for sku_name, sku in sorted(release_skus.items()):
    if not isinstance(sku, dict):
        fail(f"docs/release-skus.json entry must be an object: {sku_name}")
    if sku_name not in text:
        fail(f"release notes must mention release SKU {sku_name}")
    components = sku.get("components")
    if not isinstance(components, list):
        fail(f"docs/release-skus.json entry must declare components: {sku_name}")
    for component in components:
        if not isinstance(component, str):
            fail(f"docs/release-skus.json component entry must be a string: {sku_name}")
        if component not in text:
            fail(f"release notes must mention {sku_name} component {component}")

for phrase in ["modules-first", "Generated headers are staged release artifacts", "stage-release-artifacts.sh"]:
    if phrase not in text:
        fail(f"release notes must document modules-first release artifacts: {phrase}")

print("check-release-notes: ok")
