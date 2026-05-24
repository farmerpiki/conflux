#!/usr/bin/env python3
"""Verify direct SIMD objects do not retain AVX2 CPU-probe relocations."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


FORBIDDEN = "conflux_cpu_supports_avx2"


def usage() -> None:
    print(
        "usage: check-simd-direct-shape.py BUILD_DIR NM OBJECT_BASENAME...",
        file=sys.stderr,
    )


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        usage()
        return 2

    build_dir = Path(argv[1])
    nm = argv[2]
    object_names = argv[3:]

    if not build_dir.is_dir():
        print(f"simd-direct-shape: build dir not found: {build_dir}", file=sys.stderr)
        return 2
    if nm.endswith("-NOTFOUND"):
        print(f"simd-direct-shape: nm not available: {nm}", file=sys.stderr)
        return 2

    found: dict[str, list[Path]] = {name: [] for name in object_names}
    wanted = set(object_names)
    for root, _dirs, files in os.walk(build_dir):
        for file in files:
            if file in wanted:
                found[file].append(Path(root) / file)

    missing = [name for name, paths in found.items() if not paths]
    if missing:
        print(
            "simd-direct-shape: missing expected object(s): " + ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    offenders: list[tuple[Path, str]] = []
    for paths in found.values():
        for obj in paths:
            proc = subprocess.run(
                [nm, "-u", str(obj)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if proc.returncode not in (0, 1):
                print(
                    f"simd-direct-shape: nm failed for {obj}:\n{proc.stdout}",
                    file=sys.stderr,
                )
                return proc.returncode
            for line in proc.stdout.splitlines():
                if FORBIDDEN in line:
                    offenders.append((obj, line.strip()))

    if offenders:
        print(
            "simd-direct-shape: direct SIMD object(s) still reference " + FORBIDDEN,
            file=sys.stderr,
        )
        for obj, line in offenders:
            print(f"  {obj}: {line}", file=sys.stderr)
        return 1

    inspected = sum(len(paths) for paths in found.values())
    print(f"simd-direct-shape: inspected {inspected} object(s); no {FORBIDDEN} references")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
