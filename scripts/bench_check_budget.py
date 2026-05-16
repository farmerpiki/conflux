#!/usr/bin/env python3
"""Merge gate for recorded benchmark regressions.

Compares two recorded `runs.id` values using `bench_budget_eval`, which is
created by scripts/bench_db_migrate.sql. Exits non-zero for regressions,
unbudgeted rows, noisy rows that need rerun, missing data, or machine mismatch.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Any

DEFAULT_PGURI = "postgres://postgres@localhost/conflux_bench"
PASS_STATUSES = {"pass", "improved"}


@dataclass(frozen=True)
class RunMeta:
    run_id: int
    name: str
    benchmark: str
    config_name: str
    machine_id: str
    host: str
    artifact_dir: str


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"expected integer, got {value!r}") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError(f"expected positive integer, got {value!r}")
    return parsed


def run_psql_json(pguri: str, sql: str) -> Any:
    psql = shutil.which("psql")
    if psql is None:
        raise RuntimeError("required tool not found in PATH: psql")

    proc = subprocess.run(
        [psql, pguri, "-X", "-q", "-t", "-A", "-v", "ON_ERROR_STOP=1", "-c", sql],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"psql exited {proc.returncode}")

    text = proc.stdout.strip()
    if not text:
        return None
    return json.loads(text)


def load_run_meta(pguri: str, run_ids: tuple[int, int]) -> dict[int, RunMeta]:
    ids = ",".join(str(i) for i in run_ids)
    sql = f"""
      SELECT COALESCE(jsonb_agg(jsonb_build_object(
               'run_id', id,
               'name', name,
               'benchmark', benchmark,
               'config_name', config_name,
               'machine_id', COALESCE(machine_id, ''),
               'host', host,
               'artifact_dir', COALESCE(metadata->>'artifact_dir', '')
             ) ORDER BY id), '[]'::jsonb)::text
      FROM runs
      WHERE id IN ({ids});
    """
    rows = run_psql_json(pguri, sql) or []
    return {
        int(row["run_id"]): RunMeta(
            run_id=int(row["run_id"]),
            name=str(row["name"]),
            benchmark=str(row["benchmark"]),
            config_name=str(row["config_name"]),
            machine_id=str(row["machine_id"]),
            host=str(row["host"]),
            artifact_dir=str(row["artifact_dir"]),
        )
        for row in rows
    }


def load_budget_rows(pguri: str, baseline_run_id: int, candidate_run_id: int) -> list[dict[str, Any]]:
    sql = f"""
      SELECT COALESCE(jsonb_agg(to_jsonb(t)
               ORDER BY t.status_sort,
                        abs(COALESCE(t.pct_change, 0)) DESC,
                        t.benchmark,
                        t.config_name,
                        t.variant), '[]'::jsonb)::text
      FROM (
        SELECT e.run_a,
               e.run_b,
               e.benchmark,
               e.config_name,
               e.variant,
               e.a_med_ns,
               e.b_med_ns,
               e.a_mad,
               e.b_mad,
               e.a_mad_pct,
               e.b_mad_pct,
               e.pct_change,
               e.reps,
               e.max_regression_pct,
               e.min_samples,
               e.max_mad_pct,
               e.status,
               CASE e.status
                 WHEN 'fail' THEN 0
                 WHEN 'noisy' THEN 1
                 WHEN 'unbudgeted' THEN 2
                 WHEN 'pass' THEN 3
                 WHEN 'improved' THEN 4
                 ELSE 5
               END AS status_sort
        FROM bench_budget_eval e
        WHERE e.run_a = {baseline_run_id}
          AND e.run_b = {candidate_run_id}
      ) AS t;
    """
    rows = run_psql_json(pguri, sql) or []
    return list(rows)


def fmt_float(value: Any, digits: int = 2) -> str:
    if value is None:
        return "n/a"
    try:
        return f"{float(value):.{digits}f}"
    except (TypeError, ValueError):
        return str(value)


def fmt_pct(value: Any) -> str:
    return f"{fmt_float(value)}%" if value is not None else "n/a"


def print_run(label: str, meta: RunMeta | None) -> None:
    if meta is None:
        print(f"{label}: missing run metadata")
        return
    artifact = meta.artifact_dir or "n/a"
    print(
        f"{label}: run_id={meta.run_id} name={meta.name} "
        f"bench={meta.benchmark}/{meta.config_name} machine={meta.machine_id or 'n/a'} "
        f"host={meta.host} artifacts={artifact}"
    )


def row_noise_reason(row: dict[str, Any]) -> str:
    reasons: list[str] = []
    if row.get("pct_change") is None:
        reasons.append("delta unavailable")

    reps = row.get("reps")
    min_samples = row.get("min_samples")
    if reps is not None and min_samples is not None and int(reps) < int(min_samples):
        reasons.append(f"reps {reps} < {min_samples}")

    max_mad = row.get("max_mad_pct")
    for key, label in (("a_mad_pct", "baseline MAD"), ("b_mad_pct", "candidate MAD")):
        value = row.get(key)
        if value is not None and max_mad is not None and float(value) > float(max_mad):
            reasons.append(f"{label} {fmt_pct(value)} > {fmt_pct(max_mad)}")

    return "; ".join(reasons) if reasons else "-"


def print_table(rows: list[dict[str, Any]]) -> None:
    headers = ["status", "benchmark", "config", "variant", "delta", "budget", "reps", "noise"]
    rendered: list[list[str]] = []
    for row in rows:
        rendered.append(
            [
                str(row.get("status", "unknown")),
                str(row.get("benchmark", "")),
                str(row.get("config_name", "")),
                str(row.get("variant", "")),
                fmt_pct(row.get("pct_change")),
                fmt_pct(row.get("max_regression_pct")),
                str(row.get("reps", "n/a")),
                row_noise_reason(row),
            ]
        )

    widths = [len(header) for header in headers]
    for row in rendered:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    print("  ".join(header.ljust(widths[i]) for i, header in enumerate(headers)))
    print("  ".join("-" * widths[i] for i in range(len(headers))))
    for row in rendered:
        print("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)))


def write_json(path: str, payload: dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check benchmark regressions against DB budgets.")
    parser.add_argument("--baseline-run-id", required=True, type=positive_int)
    parser.add_argument("--candidate-run-id", required=True, type=positive_int)
    parser.add_argument("--pguri", default=os.environ.get("PGURI", DEFAULT_PGURI))
    parser.add_argument("--allow-noisy", action="store_true", help="do not fail solely because rows are noisy")
    parser.add_argument("--allow-unbudgeted", action="store_true", help="do not fail solely because rows lack budget rules")
    parser.add_argument("--json-out", help="write machine-readable report JSON to this path")
    args = parser.parse_args(argv)

    try:
        meta = load_run_meta(args.pguri, (args.baseline_run_id, args.candidate_run_id))
        baseline = meta.get(args.baseline_run_id)
        candidate = meta.get(args.candidate_run_id)
        rows = load_budget_rows(args.pguri, args.baseline_run_id, args.candidate_run_id)
    except RuntimeError as exc:
        print(f"bench budget check error: {exc}", file=sys.stderr)
        return 2

    print_run("baseline ", baseline)
    print_run("candidate", candidate)

    errors: list[str] = []
    if baseline is None or candidate is None:
        errors.append("run metadata missing")
    elif baseline.machine_id != candidate.machine_id or baseline.host != candidate.host:
        errors.append(
            "machine mismatch: "
            f"baseline={baseline.machine_id or 'n/a'}@{baseline.host} "
            f"candidate={candidate.machine_id or 'n/a'}@{candidate.host}"
        )

    if not rows:
        errors.append("no matching summary rows in bench_budget_eval")
    else:
        print()
        print_table(rows)

    counts: dict[str, int] = {}
    for row in rows:
        status = str(row.get("status", "unknown"))
        counts[status] = counts.get(status, 0) + 1

    bad_statuses = {status for status in counts if status not in PASS_STATUSES}
    if args.allow_noisy:
        bad_statuses.discard("noisy")
    if args.allow_unbudgeted:
        bad_statuses.discard("unbudgeted")

    if bad_statuses:
        errors.append(
            "blocking statuses: "
            + ", ".join(f"{status}={counts[status]}" for status in sorted(bad_statuses))
        )

    payload = {
        "baseline": baseline.__dict__ if baseline else None,
        "candidate": candidate.__dict__ if candidate else None,
        "counts": counts,
        "errors": errors,
        "rows": rows,
    }
    if args.json_out:
        write_json(args.json_out, payload)
        print(f"\njson_report={args.json_out}")

    if errors:
        print("\nFAIL: " + "; ".join(errors))
        return 1

    print("\nPASS: benchmark budget check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
