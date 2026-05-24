#!/usr/bin/env python3
"""Summarize JSON compare-bins artifacts in stable, row-oriented tables."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass


RUN_RE = re.compile(r"^\s*(?P<label>\S+)\s+.*run_id=(?P<run_id>[0-9]+)\s*$")
@dataclass(frozen=True)
class Run:
    condition: str
    profile: str
    benchmark: str
    candidate: str
    run_id: int
    log_name: str


@dataclass(frozen=True)
class Result:
    variant: str
    best: float
    p10: float
    p50: float
    p99: float
    samples: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_dir", type=pathlib.Path)
    parser.add_argument("--pguri", default="postgresql:///conflux_bench?user=postgres")
    parser.add_argument("--base-candidate", default="base")
    parser.add_argument("--wall-threshold-pct", type=float, default=1.0)
    parser.add_argument("--include-negligible", action="store_true")
    parser.add_argument("--bench", action="append", help="restrict to benchmark name; repeatable")
    parser.add_argument("--profile", action="append", help="restrict to profile substring; repeatable")
    return parser.parse_args()


def split_log_name(path: pathlib.Path) -> tuple[str, str] | None:
    stem = path.name.removesuffix(".log")
    if stem.startswith("calibrate."):
        return None
    parts = stem.split(".")
    if len(parts) < 3 or parts[0] != "combo":
        return None
    benchmark = parts[-1]
    profile = ".".join(parts[1:-1])
    return profile, benchmark


def parse_run_label(label: str, profile: str) -> tuple[str, str, str] | None:
    prefixes = (
        ("normal", f"normal-release-{profile}-", f"release-{profile}"),
        ("o2-lto", f"o2-lto-o2-lto-{profile}-", f"o2-lto-{profile}"),
        ("pgo", f"pgo-pgo-use-{profile}-", f"pgo-use-{profile}"),
    )
    for condition, prefix, run_profile in prefixes:
        if label.startswith(prefix):
            return condition, run_profile, label.removeprefix(prefix)
    return None


def parse_runs(artifact_dir: pathlib.Path) -> list[Run]:
    logs = artifact_dir / "logs"
    runs: list[Run] = []
    for log in sorted(logs.glob("*.log")):
        split = split_log_name(log)
        if split is None:
            continue
        log_profile, benchmark = split
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            match = RUN_RE.match(line)
            if not match:
                continue
            label = match.group("label")
            parsed = parse_run_label(label, log_profile)
            if parsed is None:
                continue
            condition, profile, candidate = parsed
            runs.append(
                Run(
                    condition=condition,
                    profile=profile,
                    benchmark=benchmark,
                    candidate=candidate,
                    run_id=int(match.group("run_id")),
                    log_name=log.name,
                )
            )
    return runs


def psql_rows(pguri: str, run_ids: list[int]) -> dict[int, list[Result]]:
    if not run_ids:
        return {}
    run_id_list = ",".join(str(run_id) for run_id in run_ids)
    sql = """
      select run_id, variant, best, p10, p50, p99, sample_count
      from results
      where metric = 'ns_per_iter'
        and sample_count is not null
        and run_id = any(string_to_array('__RUN_IDS__', ',')::int[])
      order by run_id, variant
    """.replace("__RUN_IDS__", run_id_list)
    cp = subprocess.run(
        [
            "psql",
            pguri,
            "-X",
            "-q",
            "-F",
            "\t",
            "-Atc",
            sql,
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if cp.returncode != 0:
        sys.stderr.write(cp.stderr)
        raise SystemExit(cp.returncode)

    out: dict[int, list[Result]] = {}
    for row in csv.reader(cp.stdout.splitlines(), delimiter="\t"):
        if len(row) != 7:
            continue
        run_id = int(row[0])
        out.setdefault(run_id, []).append(
            Result(
                variant=row[1],
                best=float(row[2]),
                p10=float(row[3]),
                p50=float(row[4]),
                p99=float(row[5]),
                samples=int(row[6]),
            )
        )
    return out


def perf_stats(artifact_dir: pathlib.Path) -> dict[tuple[str, str, str, str], dict[str, float]]:
    out: dict[tuple[str, str, str, str], dict[str, float]] = {}
    perf_dir = artifact_dir / "perf"
    for path in perf_dir.glob("*.perf.json"):
        parts = path.name.removesuffix(".perf.json").split(".")
        if len(parts) < 4:
            continue
        condition = parts[0]
        profile = parts[1]
        benchmark = parts[2]
        candidate = parts[3]
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        events = data.get("events", {})
        out[(condition, profile, benchmark, candidate)] = {
            "instructions": float(events.get("instructions_u") or events.get("instructions") or 0.0),
            "cycles": float(events.get("cycles_u") or events.get("cycles") or 0.0),
            "cache_misses": float(events.get("cache_misses_u") or events.get("cache_misses") or 0.0),
            "branch_misses": float(events.get("branch_misses_u") or events.get("branch_misses") or 0.0),
        }
    return out


def fmt(value: float, suffix: str = "") -> str:
    return f"{value:+.2f}{suffix}"


def fmt_count(value: float) -> str:
    return f"{value:+.0f}"


def print_table(rows: list[list[str]]) -> None:
    widths = [max(len(row[i]) for row in rows) for i in range(len(rows[0]))]
    for index, row in enumerate(rows):
        rendered = "  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row))
        print(rendered.rstrip())
        if index == 0:
            print("  ".join("-" * width for width in widths))


def run() -> int:
    args = parse_args()
    runs = parse_runs(args.artifact_dir)
    if args.bench:
        wanted = set(args.bench)
        runs = [run for run in runs if run.benchmark in wanted]
    if args.profile:
        runs = [run for run in runs if any(part in run.profile for part in args.profile)]
    if not runs:
        print(f"no compare-bins runs found in {args.artifact_dir}", file=sys.stderr)
        return 1

    results = psql_rows(args.pguri, [run.run_id for run in runs])
    perf = perf_stats(args.artifact_dir)
    grouped: dict[tuple[str, str, str], list[Run]] = {}
    for item in runs:
        grouped.setdefault((item.condition, item.profile, item.benchmark), []).append(item)

    for key in sorted(grouped):
        condition, profile, benchmark = key
        items = sorted(grouped[key], key=lambda item: item.candidate)
        base = next((item for item in items if item.candidate == args.base_candidate), None)
        if base is None:
            continue
        base_rows = {row.variant: row for row in results.get(base.run_id, [])}
        base_perf = perf.get((condition, profile, benchmark, base.candidate), {})
        table = [[
            "candidate",
            "variant",
            "best ns",
            "p10 ns",
            "p50 %",
            "p99 %",
            "instr %",
            "cycles %",
            "cache miss %",
            "cache miss #",
            "branch miss %",
            "branch miss #",
            "samples",
        ]]
        for item in items:
            if item.candidate == base.candidate:
                continue
            item_rows = {row.variant: row for row in results.get(item.run_id, [])}
            item_perf = perf.get((condition, profile, benchmark, item.candidate), {})
            instr_pct = None
            cycles_pct = None
            cache_miss_pct = None
            cache_miss_abs = None
            branch_miss_pct = None
            branch_miss_abs = None
            if base_perf.get("instructions") and item_perf.get("instructions"):
                instr_pct = 100.0 * (item_perf["instructions"] / base_perf["instructions"] - 1.0)
            if base_perf.get("cycles") and item_perf.get("cycles"):
                cycles_pct = 100.0 * (item_perf["cycles"] / base_perf["cycles"] - 1.0)
            if base_perf.get("cache_misses") and item_perf.get("cache_misses"):
                cache_miss_pct = 100.0 * (item_perf["cache_misses"] / base_perf["cache_misses"] - 1.0)
                cache_miss_abs = item_perf["cache_misses"] - base_perf["cache_misses"]
            if base_perf.get("branch_misses") and item_perf.get("branch_misses"):
                branch_miss_pct = 100.0 * (item_perf["branch_misses"] / base_perf["branch_misses"] - 1.0)
                branch_miss_abs = item_perf["branch_misses"] - base_perf["branch_misses"]

            for variant in sorted(set(base_rows) & set(item_rows)):
                baseline = base_rows[variant]
                current = item_rows[variant]
                p50_pct = 100.0 * (current.p50 / baseline.p50 - 1.0)
                if (
                    not args.include_negligible
                    and abs(p50_pct) < args.wall_threshold_pct
                ):
                    continue
                table.append(
                    [
                        item.candidate,
                        variant,
                        f"{current.best - baseline.best:+.1f}",
                        f"{current.p10 - baseline.p10:+.1f}",
                        fmt(p50_pct, "%"),
                        fmt(100.0 * (current.p99 / baseline.p99 - 1.0), "%"),
                        "n/a" if instr_pct is None else fmt(instr_pct, "%"),
                        "n/a" if cycles_pct is None else fmt(cycles_pct, "%"),
                        "n/a" if cache_miss_pct is None else fmt(cache_miss_pct, "%"),
                        "n/a" if cache_miss_abs is None else fmt_count(cache_miss_abs),
                        "n/a" if branch_miss_pct is None else fmt(branch_miss_pct, "%"),
                        "n/a" if branch_miss_abs is None else fmt_count(branch_miss_abs),
                        str(current.samples),
                    ]
                )

        if len(table) == 1:
            continue
        print(f"\n[{condition}] {profile} / {benchmark}")
        print_table(table)

    return 0


if __name__ == "__main__":
    raise SystemExit(run())
