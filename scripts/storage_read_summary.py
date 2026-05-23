#!/usr/bin/env python3
"""Summarize storage_read_bench NDJSON evidence.

The storage-read evidence wrapper records every repeated launch as raw NDJSON.
This script creates a compact gate summary per config/mode and computes the key
ratios needed before promoting IOPOLL/static-file claims: iopoll_read_fixed vs
pread, io_uring_read, and read_fixed.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

IMPORTANT_VARIANTS = ("pread", "io_uring_read", "read_fixed", "iopoll_read_fixed")


@dataclass(frozen=True)
class GroupKey:
    config: str
    variant: str


def load_rows(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as fh:
        for line_no, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{line_no}: invalid JSON: {exc}") from exc
            if not isinstance(row, dict):
                raise SystemExit(f"{path}:{line_no}: expected object row")
            if "config" not in row or "variant" not in row:
                raise SystemExit(f"{path}:{line_no}: row lacks config/variant")
            rows.append(row)
    return rows


def numeric_values(rows: Iterable[dict[str, Any]], field: str) -> list[float]:
    values: list[float] = []
    for row in rows:
        value = row.get(field)
        if isinstance(value, bool) or value is None:
            continue
        try:
            values.append(float(value))
        except (TypeError, ValueError):
            continue
    return values


def median_or_none(values: list[float]) -> float | None:
    if not values:
        return None
    return float(statistics.median(values))


def min_or_none(values: list[float]) -> float | None:
    return min(values) if values else None


def max_or_none(values: list[float]) -> float | None:
    return max(values) if values else None


def summarize_group(key: GroupKey, rows: list[dict[str, Any]]) -> dict[str, Any]:
    ns = numeric_values(rows, "ns_per_iter")
    mibs = numeric_values(rows, "mib_per_s")
    bytes_values = numeric_values(rows, "bytes")
    operations = numeric_values(rows, "operations") or numeric_values(rows, "iterations")
    direct_reads = numeric_values(rows, "direct_reads")
    fallbacks = numeric_values(rows, "fallbacks")

    first = rows[0]
    perf_cycles = numeric_values(
        (row.get("perf_derived", {}) for row in rows if isinstance(row.get("perf_derived"), dict)),
        "cycles_per_op",
    )
    perf_instructions = numeric_values(
        (row.get("perf_derived", {}) for row in rows if isinstance(row.get("perf_derived"), dict)),
        "instructions_per_op",
    )
    perf_context_switches = numeric_values(
        (row.get("perf_derived", {}) for row in rows if isinstance(row.get("perf_derived"), dict)),
        "context_switches_per_op",
    )

    return {
        "config": key.config,
        "variant": key.variant,
        "reps": len(rows),
        "depth": first.get("depth", first.get("requested_depth")),
        "chunk": first.get("chunk", first.get("requested_chunk")),
        "requested_modes": first.get("requested_modes"),
        "bytes_median": median_or_none(bytes_values),
        "operations_median": median_or_none(operations),
        "ns_per_iter_min": min_or_none(ns),
        "ns_per_iter_median": median_or_none(ns),
        "ns_per_iter_max": max_or_none(ns),
        "mib_per_s_min": min_or_none(mibs),
        "mib_per_s_median": median_or_none(mibs),
        "mib_per_s_max": max_or_none(mibs),
        "direct_reads_median": median_or_none(direct_reads),
        "fallbacks_median": median_or_none(fallbacks),
        "perf_cycles_per_op_median": median_or_none(perf_cycles),
        "perf_instructions_per_op_median": median_or_none(perf_instructions),
        "perf_context_switches_per_op_median": median_or_none(perf_context_switches),
        "label": first.get("label"),
        "evidence_role": first.get("evidence_role"),
    }


def ratio(numerator: float | None, denominator: float | None) -> float | None:
    if numerator is None or denominator is None or denominator == 0.0:
        return None
    return numerator / denominator


def build_config_comparisons(summaries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_config: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    for item in summaries:
        by_config[str(item["config"])][str(item["variant"])] = item

    comparisons: list[dict[str, Any]] = []
    for config, variants in sorted(by_config.items()):
        throughput = {
            variant: item.get("mib_per_s_median") for variant, item in variants.items()
        }
        winner = None
        winner_value = None
        for variant, value in throughput.items():
            if isinstance(value, (int, float)) and (winner_value is None or value > winner_value):
                winner = variant
                winner_value = float(value)
        iopoll = variants.get("iopoll_read_fixed", {}).get("mib_per_s_median")
        comparisons.append(
            {
                "config": config,
                "present_variants": sorted(variants),
                "missing_core_variants": [v for v in IMPORTANT_VARIANTS if v not in variants],
                "winner_by_mib_per_s_median": winner,
                "winner_mib_per_s_median": winner_value,
                "iopoll_read_fixed_mib_per_s_median": iopoll,
                "iopoll_vs_pread_mib_ratio": ratio(iopoll, variants.get("pread", {}).get("mib_per_s_median")),
                "iopoll_vs_io_uring_read_mib_ratio": ratio(
                    iopoll, variants.get("io_uring_read", {}).get("mib_per_s_median")
                ),
                "iopoll_vs_read_fixed_mib_ratio": ratio(
                    iopoll, variants.get("read_fixed", {}).get("mib_per_s_median")
                ),
                "iopoll_fallbacks_median": variants.get("iopoll_read_fixed", {}).get("fallbacks_median"),
                "iopoll_direct_reads_median": variants.get("iopoll_read_fixed", {}).get("direct_reads_median"),
            }
        )
    return comparisons


def check_expected_configs(rows: list[dict[str, Any]], expected: list[str]) -> list[str]:
    present = {str(row.get("config", "")) for row in rows}
    return [name for name in expected if name not in present]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ndjson", type=Path, help="storage_read raw NDJSON")
    parser.add_argument("--output", type=Path, help="write summary JSON")
    parser.add_argument(
        "--expected-config",
        action="append",
        default=[],
        help="config name that must appear at least once; repeatable",
    )
    args = parser.parse_args()

    rows = load_rows(args.ndjson)
    if not rows:
        raise SystemExit(f"{args.ndjson}: no rows")

    grouped: dict[GroupKey, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[GroupKey(str(row.get("config")), str(row.get("variant")))].append(row)

    summaries = [summarize_group(key, group) for key, group in sorted(grouped.items(), key=lambda item: (item[0].config, item[0].variant))]
    missing_expected = check_expected_configs(rows, args.expected_config)
    output = {
        "artifact_kind": "storage_read_summary",
        "source": str(args.ndjson),
        "row_count": len(rows),
        "configs": sorted({str(row.get("config")) for row in rows}),
        "variants": sorted({str(row.get("variant")) for row in rows}),
        "expected_configs": args.expected_config,
        "missing_expected_configs": missing_expected,
        "summaries": summaries,
        "comparisons": build_config_comparisons(summaries),
        "claim_rule": "Do not promote IOPOLL/static-file storage claims unless raw rows, this summary, host metadata, and perf counters support the specific path.",
    }

    text = json.dumps(output, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    if missing_expected:
        print(f"missing expected storage_read configs: {', '.join(missing_expected)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
