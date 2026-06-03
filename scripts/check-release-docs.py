#!/usr/bin/env python3
"""Validate the first-contact release docs path."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    print(f"check-release-docs: {message}", file=sys.stderr)
    raise SystemExit(1)


readme = (ROOT / "README.md").read_text(encoding="utf-8")
for rel in [
    "SECURITY.md",
    "SUPPORT.md",
    "CHANGELOG.md",
    "RELEASE_POLICY.md",
]:
    if rel not in readme:
        fail(f"README.md does not link {rel}")
    text = (ROOT / rel).read_text(encoding="utf-8")
    if "pre-v1" not in text and "pre-1.0" not in text:
        fail(f"{rel} must state the prerelease support/versioning context")
    if rel != "CHANGELOG.md" and "docs/release-checklist.md" not in text:
        fail(f"{rel} must link the release checklist")

required = [
    "docs/package-consumption.md",
    "docs/component-map.md",
    "docs/public-api-map.md",
    "docs/http-server-api.md",
    "docs/json-api.md",
    "docs/cost-lifetime-model.md",
    "docs/production-checklist.md",
    "docs/release-checklist.md",
]
for rel in required:
    if rel not in readme:
        fail(f"README.md does not link {rel}")
    if not (ROOT / rel).exists():
        fail(f"README.md links missing file {rel}")

first_contact = readme.split("## Project policy", 1)[0]
if "todo/" in first_contact or "proposals/" in first_contact:
    fail("README first-contact path must not route users to TODO/proposal files")

status = (ROOT / "docs/prerelease-status.md").read_text(encoding="utf-8")
for phrase in [
    "core",
    "json",
    "file_io_sync",
    "runtime",
    "http",
    "real-liburing",
    "DB-off",
    "MODULE_INTERFACE",
    "Generated headers are release artifacts",
    "stage-release-artifacts.sh",
]:
    if phrase not in status:
        fail(f"docs/prerelease-status.md must mention {phrase}")

for phrase in ["modules-first", "Generated headers are staged release artifacts"]:
    if phrase not in readme:
        fail(f"README.md must document modules-first release artifacts: {phrase}")

for match in re.finditer(r"\]\(([^)]+)\)", readme):
    target = match.group(1)
    if target.startswith(("http://", "https://", "#")):
        continue
    if not (ROOT / target).exists():
        fail(f"README.md link target missing: {target}")

print("check-release-docs: ok")
