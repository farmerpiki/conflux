#!/usr/bin/env python3
"""Record build cost baseline: binary sizes and library archive sizes.

First-preview mode: record-only. No pass/fail budget enforced.
Run after building the selected release preset.

Usage: measure-build-costs.py <build-dir> [--sku <sku>] [--json]
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys


def file_size(path: pathlib.Path) -> int | None:
    try:
        return path.stat().st_size
    except OSError:
        return None


def git_commit(root: pathlib.Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return "unknown"


def compiler_info(build_dir: pathlib.Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    info: dict[str, str] = {}
    if not cache.exists():
        return info
    for line in cache.read_text(errors="replace").splitlines():
        for key in ("CMAKE_CXX_COMPILER:STRING", "CMAKE_CXX_COMPILER:FILEPATH",
                    "CMAKE_BUILD_TYPE:STRING", "CONFLUX_ENABLE_LTO:BOOL",
                    "CONFLUX_LTO_MODE:STRING", "CONFLUX_INTERFACE_MODE:STRING",
                    "CONFLUX_FEATURE_SET:STRING"):
            if line.startswith(key + "="):
                canon = key.split(":")[0]
                if canon not in info:
                    info[canon] = line.split("=", 1)[1]
    return info


BINARY_TARGETS: list[tuple[str, str]] = [
    ("hello", "examples/conflux_hello"),
    ("production-showcase", "examples/conflux_production_showcase_example"),
    ("json-bench", "benchmarks/conflux_json_bench"),
    ("http-server-bench", "benchmarks/conflux_http_server_bench"),
]

LIB_TARGETS: list[tuple[str, str]] = [
    ("json", "lib/libconflux_json.a"),
    ("http-server", "lib/libconflux_http_server.a"),
    ("http-app", "lib/libconflux_http_app.a"),
    ("aggregate", "lib/libconflux.a"),
    ("work", "lib/libconflux_work.a"),
    ("uring", "lib/libconflux_uring.a"),
    ("crypto", "lib/libconflux_crypto.a"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build_dir", help="CMake build directory")
    ap.add_argument("--sku", default="unknown", help="Release SKU name")
    ap.add_argument("--json", action="store_true", help="Output JSON")
    args = ap.parse_args()

    build = pathlib.Path(args.build_dir).resolve()
    root = pathlib.Path(__file__).resolve().parents[1]

    record: dict = {
        "conflux_commit": git_commit(root),
        "sku": args.sku,
        "build_dir": str(build),
        "compiler": compiler_info(build),
        "binaries": {},
        "libraries": {},
    }

    for name, rel in BINARY_TARGETS:
        p = build / rel
        sz = file_size(p)
        record["binaries"][name] = {"path": rel, "bytes": sz}

    for name, rel in LIB_TARGETS:
        p = build / rel
        sz = file_size(p)
        record["libraries"][name] = {"path": rel, "bytes": sz}

    if args.json:
        print(json.dumps(record, indent=2))
        return 0

    commit = record["conflux_commit"]
    sku = record["sku"]
    cxx = record["compiler"].get("CMAKE_CXX_COMPILER", "?")
    btype = record["compiler"].get("CMAKE_BUILD_TYPE", "?")
    lto_on = record["compiler"].get("CONFLUX_ENABLE_LTO", "?")
    lto_mode = record["compiler"].get("CONFLUX_LTO_MODE", "")
    lto = f"{lto_on}/{lto_mode}" if lto_mode else lto_on
    mode = record["compiler"].get("CONFLUX_INTERFACE_MODE", "?")
    fset = record["compiler"].get("CONFLUX_FEATURE_SET", "?")
    print(f"conflux {commit}  sku={sku}  compiler={os.path.basename(cxx)}  type={btype}  lto={lto}  mode={mode}  feature_set={fset}")
    print()
    print(f"{'Binary':<30} {'Bytes':>12}  {'KB':>8}")
    print("-" * 55)
    for name, d in record["binaries"].items():
        sz = d["bytes"]
        if sz is None:
            print(f"  {name:<28} {'(not built)':>12}")
        else:
            print(f"  {name:<28} {sz:>12,}  {sz//1024:>6} KB")
    print()
    print(f"{'Library archive':<30} {'Bytes':>12}  {'KB':>8}")
    print("-" * 55)
    for name, d in record["libraries"].items():
        sz = d["bytes"]
        if sz is None:
            print(f"  {name:<28} {'(not built)':>12}")
        else:
            print(f"  {name:<28} {sz:>12,}  {sz//1024:>6} KB")
    print()
    print("Note: first-preview baseline — record only, no budget enforced.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
