#!/usr/bin/env python3
"""Summarize workpool_queue_mode_compare queue-contention NDJSON."""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

REQUIRED_VARIANTS = (
    "stealing/single_thread",
    "no_stealing/single_thread",
    "stealing/contended",
    "no_stealing/contended",
    "stealing/external_burst",
    "no_stealing/external_burst",
    "stealing/local_fanout",
    "no_stealing/local_fanout",
    "stealing/local_backlog_redistribution",
    "no_stealing/local_backlog_redistribution",
)
PROFILE_VARIANTS = ("single_thread", "contended", "external_burst", "local_fanout", "local_backlog_redistribution")

REQUIRED_QUEUE_KEYS = (
    "enqueue_attempts",
    "admission_lock_acquisitions",
    "admission_lock_contentions",
    "local_lock_acquisitions",
    "local_lock_contentions",
    "steal_lock_acquisitions",
    "steal_lock_contentions",
    "local_pushes",
    "inject_pushes",
    "steal_hits",
    "jobs_run",
    "wake_one_futex_wakes",
    "wake_all_futex_wakes",
    "park_attempts",
    "park_recheck_skips",
    "futex_waits",
)


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(2)


def as_number(row: dict[str, Any], key: str, line_no: int) -> float:
    value = row.get(key)
    if not isinstance(value, (int, float)):
        fail(f"line {line_no}: {key} must be numeric")
    return float(value)


def as_non_negative_int(value: Any, key: str, line_no: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        fail(f"line {line_no}: queue.{key} must be a non-negative integer")
    return value


def median(values: list[float]) -> float:
    return float(statistics.median(values))


def rate_per_1k(numerator: int, denominator: int) -> float:
    if denominator <= 0:
        return 0.0
    return round((float(numerator) * 1000.0) / float(denominator), 6)


def median_numeric(values: list[Any]) -> float:
    nums = [float(value) for value in values if isinstance(value, (int, float)) and not isinstance(value, bool)]
    if not nums:
        return 0.0
    return median(nums)


def read_rows(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as src:
        for line_no, line in enumerate(src, 1):
            text = line.strip()
            if not text:
                continue
            try:
                row = json.loads(text)
            except json.JSONDecodeError as exc:
                fail(f"line {line_no}: invalid JSON: {exc}")
            if not isinstance(row, dict):
                fail(f"line {line_no}: row must be a JSON object")
            config = row.get("config")
            variant = row.get("variant")
            if not isinstance(config, str) or not config:
                fail(f"line {line_no}: config must be a non-empty string")
            if not isinstance(variant, str) or not variant:
                fail(f"line {line_no}: variant must be a non-empty string")
            iterations = as_number(row, "iterations", line_no)
            total_ns = as_number(row, "total_ns", line_no)
            ns_per_iter = as_number(row, "ns_per_iter", line_no)
            if iterations <= 0 or total_ns < 0 or ns_per_iter < 0:
                fail(f"line {line_no}: timing fields must be non-negative and iterations > 0")
            queue = row.get("queue")
            if not isinstance(queue, dict):
                fail(f"line {line_no}: missing queue object; rebuild with CONFLUX_WORK_QUEUE_STATS=ON")
            for key in REQUIRED_QUEUE_KEYS:
                queue[key] = as_non_negative_int(queue.get(key), key, line_no)
            fairness = row.get("fairness")
            if fairness is not None and not isinstance(fairness, dict):
                fail(f"line {line_no}: fairness must be an object when present")
            rows.append(row)
    if not rows:
        fail(f"no NDJSON rows found in {path}")
    return rows


def summarize(rows: list[dict[str, Any]], raw_path: Path, required_variants: set[str]) -> dict[str, Any]:
    by_config_variant: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        by_config_variant[(row["config"], row["variant"])].append(row)

    configs = sorted({row["config"] for row in rows})
    missing: list[str] = []
    for config in configs:
        present = {variant for (cfg, variant) in by_config_variant if cfg == config}
        for variant in sorted(required_variants - present):
            missing.append(f"{config}/{variant}")
    if missing:
        fail("missing required config/variant rows: " + ", ".join(missing))

    variants: list[dict[str, Any]] = []
    variant_timing: dict[tuple[str, str], list[float]] = {}
    total_queue: defaultdict[str, int] = defaultdict(int)
    for (config, variant), bucket in sorted(by_config_variant.items()):
        timing = [float(row["ns_per_iter"]) for row in bucket]
        variant_timing[(config, variant)] = timing
        queue_sums: defaultdict[str, int] = defaultdict(int)
        for row in bucket:
            for key, value in row["queue"].items():
                if isinstance(value, int):
                    queue_sums[key] += value
                    total_queue[key] += value

        jobs = queue_sums["jobs_run"]
        fairness_rows = [row.get("fairness") for row in bucket if isinstance(row.get("fairness"), dict)]
        fairness = {
            "runner_threads_median": median_numeric([row.get("runner_threads") for row in fairness_rows]),
            "child_jobs_median": median_numeric([row.get("child_jobs") for row in fairness_rows]),
            "min_runner_jobs_median": median_numeric([row.get("min_runner_jobs") for row in fairness_rows]),
            "max_runner_jobs_median": median_numeric([row.get("max_runner_jobs") for row in fairness_rows]),
            "min_runner_share_median": median_numeric([row.get("min_runner_share") for row in fairness_rows]),
            "max_runner_share_median": median_numeric([row.get("max_runner_share") for row in fairness_rows]),
        }
        variants.append(
            {
                "config": config,
                "variant": variant,
                "samples": len(bucket),
                "median_ns_per_iter": median(timing),
                "best_ns_per_iter": min(timing),
                "queue": dict(sorted(queue_sums.items())),
                "fairness": fairness,
                "rates_per_1k_jobs": {
                    "admission_lock_contentions": rate_per_1k(queue_sums["admission_lock_contentions"], jobs),
                    "local_lock_contentions": rate_per_1k(queue_sums["local_lock_contentions"], jobs),
                    "steal_lock_contentions": rate_per_1k(queue_sums["steal_lock_contentions"], jobs),
                    "futex_waits": rate_per_1k(queue_sums["futex_waits"], jobs),
                },
            }
        )

    mode_comparisons: list[dict[str, Any]] = []
    for config in configs:
        for profile in PROFILE_VARIANTS:
            stealing_timing = variant_timing.get((config, f"stealing/{profile}"))
            no_stealing_timing = variant_timing.get((config, f"no_stealing/{profile}"))
            if not stealing_timing or not no_stealing_timing:
                continue
            stealing_med = median(stealing_timing)
            no_stealing_med = median(no_stealing_timing)
            stealing_rows = by_config_variant[(config, f"stealing/{profile}")]
            no_stealing_rows = by_config_variant[(config, f"no_stealing/{profile}")]
            stealing_fairness = [row.get("fairness") for row in stealing_rows if isinstance(row.get("fairness"), dict)]
            no_stealing_fairness = [row.get("fairness") for row in no_stealing_rows if isinstance(row.get("fairness"), dict)]
            mode_comparisons.append(
                {
                    "config": config,
                    "profile": profile,
                    "stealing_median_ns_per_iter": stealing_med,
                    "no_stealing_median_ns_per_iter": no_stealing_med,
                    "no_stealing_pct_change": round(((no_stealing_med - stealing_med) * 100.0) / stealing_med, 6)
                    if stealing_med > 0
                    else 0.0,
                    "no_stealing_speedup": round(stealing_med / no_stealing_med, 6) if no_stealing_med > 0 else 0.0,
                    "stealing_runner_threads_median": median_numeric(
                        [row.get("runner_threads") for row in stealing_fairness]
                    ),
                    "no_stealing_runner_threads_median": median_numeric(
                        [row.get("runner_threads") for row in no_stealing_fairness]
                    ),
                    "stealing_max_runner_share_median": median_numeric(
                        [row.get("max_runner_share") for row in stealing_fairness]
                    ),
                    "no_stealing_max_runner_share_median": median_numeric(
                        [row.get("max_runner_share") for row in no_stealing_fairness]
                    ),
                }
            )

    jobs_run = total_queue["jobs_run"]
    enqueue_attempts = total_queue["enqueue_attempts"]
    if jobs_run == 0 and enqueue_attempts == 0:
        fail("queue counters are all zero; rebuild/run with CONFLUX_WORK_QUEUE_STATS=ON")

    return {
        "generated_utc": _dt.datetime.now(_dt.UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "raw_ndjson": str(raw_path),
        "rows": len(rows),
        "configs": configs,
        "required_variants": sorted(required_variants),
        "mode_comparisons": mode_comparisons,
        "observed": {
            "admission_lock_contention": total_queue["admission_lock_contentions"] > 0,
            "local_lock_contention": total_queue["local_lock_contentions"] > 0,
            "steal_lock_contention": total_queue["steal_lock_contentions"] > 0,
            "futex_waits": total_queue["futex_waits"] > 0,
        },
        "totals": dict(sorted(total_queue.items())),
        "rates_per_1k_jobs": {
            "admission_lock_contentions": rate_per_1k(total_queue["admission_lock_contentions"], jobs_run),
            "local_lock_contentions": rate_per_1k(total_queue["local_lock_contentions"], jobs_run),
            "steal_lock_contentions": rate_per_1k(total_queue["steal_lock_contentions"], jobs_run),
            "futex_waits": rate_per_1k(total_queue["futex_waits"], jobs_run),
        },
        "variants": variants,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_ndjson", type=Path, help="workpool_queue_mode_compare --json NDJSON")
    parser.add_argument("--output", type=Path, help="write summary JSON to this path")
    parser.add_argument(
        "--required-variant",
        action="append",
        help="required variant per config; repeatable; defaults to all queue-profile variants",
    )
    args = parser.parse_args()
    if args.required_variant is None:
        args.required_variant = list(REQUIRED_VARIANTS)
    return args


def main() -> int:
    args = parse_args()
    rows = read_rows(args.raw_ndjson)
    summary = summarize(rows, args.raw_ndjson, set(args.required_variant))
    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
