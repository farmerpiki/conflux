#!/usr/bin/env python3
"""Cheap guard for the cost/lifetime documentation entry points."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "cost-lifetime-model.md"

REQUIRED_HEADINGS = [
    "## HTTP request lifetimes",
    "## HTTP response costs",
    "## JSON ownership and allocation",
    "## File and mmap lifetimes",
    "## Runtime blocking model",
    "## Do not do this",
]

REQUIRED_DOC_MARKERS = [
    "| Type | owns memory? | borrows from? | valid until | can suspend? | can cross thread? | can be stored? | copies? | allocates? |",
    "| Response | body ownership | copies body? | allocates? | blocks? | zero-copy eligible? | TLS caveat? | validity requirement |",
    "| API | input ownership | string ownership | arena/PMR behavior | copies? | allocates? | error path allocations? | UTF-8 validation? | duplicate key behavior? |",
    "| API/type | ownership | valid until | copies? | allocates? | blocks? | zero-copy caveat |",
    "| API/path | runs on caller? | blocks caller? | blocks ring thread? | requires explicit opt-in? | safe inside HTTP handler? |",
    "`http::RequestView`",
    "`http::OwnedRequest`",
    "`http::BodyBytes`",
    "`http::OwnedBodyBytes`",
    "`http::JsonDocument`",
    "`parse_borrowed`",
    "`parse_owned`",
    "`parse_into`",
    "`parse_copy`",
    "`http::owned_text(...)`",
    "`http::owned_created(...)`",
    "`http::blocking_file_response(...)`",
    "`http::buffered_stream(...)`",
    "`file_map::MappedFileLease`",
    "Static mapped response",
    "mmap",
    "Do not block inside handlers.",
]

FORBIDDEN_PUBLIC_HELPERS = [
    "http::ok(",
    "http::ok()",
    "`http::ok",
]

REQUIRED_LINKS = {
    ROOT / "README.md": "docs/cost-lifetime-model.md",
    ROOT / "docs" / "public-api-map.md": "cost-lifetime-model.md",
    ROOT / "docs" / "release-checklist.md": "docs/cost-lifetime-model.md",
}


def main() -> int:
    failures: list[str] = []

    if not DOC.exists():
        failures.append("missing docs/cost-lifetime-model.md")
    else:
        text = DOC.read_text(encoding="utf-8")
        for heading in REQUIRED_HEADINGS:
            if heading not in text:
                failures.append(f"missing required heading: {heading}")
        for marker in REQUIRED_DOC_MARKERS:
            if marker not in text:
                failures.append(f"docs/cost-lifetime-model.md missing marker: {marker}")
        for helper in FORBIDDEN_PUBLIC_HELPERS:
            if helper in text:
                failures.append(f"docs/cost-lifetime-model.md mentions nonexistent helper: {helper}")

    for path, needle in REQUIRED_LINKS.items():
        text = path.read_text(encoding="utf-8")
        if needle not in text:
            failures.append(f"{path.relative_to(ROOT)} does not mention {needle}")

    if failures:
        print("cost-lifetime docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("cost-lifetime docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
