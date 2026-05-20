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
    "## Runtime blocking model",
    "## Do not do this",
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
