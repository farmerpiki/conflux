#!/usr/bin/env python3
"""Cheap guard for HTTP lifecycle/backpressure docs."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CHECKS = {
    ROOT / "docs" / "http-server-api.md": [
        "Lifecycle and pressure",
        "DrainOptions",
        "DrainReport",
        "OverflowPolicy",
        "HttpPressureMetrics",
    ],
    ROOT / "docs" / "execution-model.md": ["Server drain"],
    ROOT / "docs" / "cost-lifetime-model.md": ["HttpServer::drain"],
    ROOT / "docs" / "release-checklist.md": ["Lifecycle/backpressure docs"],
    ROOT / "docs" / "production-checklist.md": ["Lifecycle", "Pressure"],
}


def main() -> int:
    failures: list[str] = []
    for path, needles in CHECKS.items():
        if not path.exists():
            failures.append(f"missing {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        for needle in needles:
            if needle not in text:
                failures.append(f"{path.relative_to(ROOT)} missing {needle!r}")

    example = ROOT / "examples" / "advanced" / "http_lifecycle.cxx"
    if not example.exists():
        failures.append("missing examples/advanced/http_lifecycle.cxx")

    if failures:
        print("lifecycle docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("lifecycle docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
