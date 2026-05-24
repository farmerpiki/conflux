#!/usr/bin/env python3
"""Run one benchmark command under perf stat and annotate NDJSON rows.

This is intentionally a host-side evidence helper, not a benchmark by itself.
It keeps stdout machine-readable: JSON benchmark rows from the child are copied
through with a `perf_stat` object and derived per-iteration/per-op counters.
Non-JSON child output is copied unchanged.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

DEFAULT_EVENTS = (
    "cycles,instructions,branches,branch-misses,cache-references,cache-misses,"
    "L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses,cs,"
    "syscalls:sys_enter_io_uring_enter,syscalls:sys_enter_sendto,syscalls:sys_enter_recvfrom"
)

NUMBER_RE = re.compile(r"^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)$")


def parse_perf_csv(text: str) -> dict[str, Any]:
    metrics: dict[str, Any] = {}
    unsupported: list[str] = []
    not_counted: list[str] = []
    raw_lines: list[str] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        raw_lines.append(line)
        parts = line.split(",")
        if len(parts) < 3:
            continue
        value = parts[0].strip().replace(" ", "")
        unit = parts[1].strip()
        event = parts[2].strip()
        if not event:
            continue
        key = event.replace("/", "_").replace(":", "_").replace("-", "_")
        if "not supported" in value:
            unsupported.append(event)
            continue
        if "not counted" in value:
            not_counted.append(event)
            continue
        if NUMBER_RE.match(value):
            if "." in value:
                metrics[key] = float(value)
            else:
                metrics[key] = int(value)
            if unit:
                metrics[f"{key}_unit"] = unit
    out: dict[str, Any] = {"events": metrics}
    if unsupported:
        out["unsupported_events"] = unsupported
    if not_counted:
        out["not_counted_events"] = not_counted
    if raw_lines:
        out["raw_csv"] = raw_lines
    return out


def add_derived(row: dict[str, Any], perf: dict[str, Any]) -> dict[str, Any]:
    events = perf.get("events", {})
    denom = row.get("total_ops") or row.get("iterations")
    derived: dict[str, Any] = {}
    if isinstance(denom, int | float) and denom > 0:
        for source, dest in (
            ("cycles", "cycles_per_op"),
            ("instructions", "instructions_per_op"),
            ("branches", "branches_per_op"),
            ("cache_references", "cache_references_per_op"),
            ("cache_misses", "cache_misses_per_op"),
            ("L1_dcache_loads", "l1_dcache_loads_per_op"),
            ("L1_dcache_load_misses", "l1_dcache_load_misses_per_op"),
            ("dTLB_loads", "dtlb_loads_per_op"),
            ("dTLB_load_misses", "dtlb_load_misses_per_op"),
            ("branch_misses", "branch_misses_per_op"),
            ("cs", "context_switches_per_op"),
            ("syscalls_sys_enter_io_uring_enter", "io_uring_enter_syscalls_per_op"),
            ("syscalls_sys_enter_sendto", "sendto_syscalls_per_op"),
            ("syscalls_sys_enter_recvfrom", "recvfrom_syscalls_per_op"),
        ):
            value = events.get(source)
            if isinstance(value, int | float):
                derived[dest] = float(value) / float(denom)
    instructions = events.get("instructions")
    cycles = events.get("cycles")
    branches = events.get("branches")
    branch_misses = events.get("branch_misses")
    cache_references = events.get("cache_references")
    cache_misses = events.get("cache_misses")
    l1_loads = events.get("L1_dcache_loads")
    l1_misses = events.get("L1_dcache_load_misses")
    dtlb_loads = events.get("dTLB_loads")
    dtlb_misses = events.get("dTLB_load_misses")
    if isinstance(cycles, int | float) and isinstance(instructions, int | float) and instructions > 0:
        derived["cycles_per_instruction"] = float(cycles) / float(instructions)
    if isinstance(branch_misses, int | float) and isinstance(branches, int | float) and branches > 0:
        derived["branch_miss_rate"] = float(branch_misses) / float(branches)
    if isinstance(cache_misses, int | float) and isinstance(cache_references, int | float) and cache_references > 0:
        derived["cache_miss_rate"] = float(cache_misses) / float(cache_references)
    if isinstance(l1_misses, int | float) and isinstance(l1_loads, int | float) and l1_loads > 0:
        derived["l1_dcache_load_miss_rate"] = float(l1_misses) / float(l1_loads)
    if isinstance(dtlb_misses, int | float) and isinstance(dtlb_loads, int | float) and dtlb_loads > 0:
        derived["dtlb_load_miss_rate"] = float(dtlb_misses) / float(dtlb_loads)
    if derived:
        row["perf_derived"] = derived
    return row


def annotate_stdout(stdout: str, perf: dict[str, Any]) -> list[str]:
    out: list[str] = []
    for line in stdout.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        try:
            row = json.loads(stripped)
        except json.JSONDecodeError:
            out.append(line)
            continue
        if isinstance(row, dict):
            row["perf_stat"] = perf
            add_derived(row, perf)
            out.append(json.dumps(row, sort_keys=True, separators=(",", ":")))
        else:
            out.append(line)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run command under perf stat and annotate NDJSON rows with counters."
    )
    parser.add_argument("--events", default=os.environ.get("BENCH_PERF_EVENTS", DEFAULT_EVENTS))
    parser.add_argument("--output", type=pathlib.Path, help="write annotated stdout/NDJSON here")
    parser.add_argument("--perf-json", type=pathlib.Path, help="write parsed perf counters here")
    parser.add_argument("--perf-stderr", type=pathlib.Path, help="write raw perf stderr here")
    parser.add_argument("--allow-perf-failure", action="store_true")
    parser.add_argument("cmd", nargs=argparse.REMAINDER, help="command after --")
    args = parser.parse_args()

    cmd = args.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.error("missing command; use: bench_perf_stat.py -- <cmd> [args...]")
    if shutil.which("perf") is None:
        raise SystemExit("perf not found in PATH")

    with tempfile.TemporaryDirectory(prefix="conflux-perf-stat-") as tmp:
        perf_stderr = pathlib.Path(tmp) / "perf.stderr"
        perf_cmd = ["perf", "stat", "-x", ",", "-e", args.events, "--", *cmd]
        cp = subprocess.run(perf_cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        perf_stderr.write_text(cp.stderr, encoding="utf-8")
        perf = parse_perf_csv(cp.stderr)
        perf.update({"cmd": cmd, "events_requested": args.events, "perf_returncode": cp.returncode})

        if args.perf_stderr:
            args.perf_stderr.parent.mkdir(parents=True, exist_ok=True)
            args.perf_stderr.write_text(cp.stderr, encoding="utf-8")
        if args.perf_json:
            args.perf_json.parent.mkdir(parents=True, exist_ok=True)
            args.perf_json.write_text(json.dumps(perf, indent=2, sort_keys=True) + "\n", encoding="utf-8")

        lines = annotate_stdout(cp.stdout, perf)
        rendered = "\n".join(lines) + ("\n" if lines else "")
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
        sys.stdout.write(rendered)

        if cp.returncode != 0 and not args.allow_perf_failure:
            return cp.returncode
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
