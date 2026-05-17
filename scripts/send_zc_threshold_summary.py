#!/usr/bin/env python3
"""Summarize conflux_send_zc_bench threshold NDJSON."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

COUNTER_KEYS = (
    "zc_attempts",
    "zc_plain_attempts",
    "zc_mapped_attempts",
    "zc_bytes_requested",
    "zc_bytes_sent",
    "zc_notifications",
    "zc_copied_notifications",
    "zc_sends_without_notification",
    "zc_errors_enomem",
    "zc_errors_other",
    "zc_fallback_regular_send",
    "zc_tls_bypass",
    "zc_tls_bypass_bytes",
    "zc_adaptive_disable_count",
    "zc_notifications_pending",
)

TIMING_KEYS = ("iterations", "total_ns", "ns_per_iter")
MODE_RE = re.compile(r"^(?P<stem>.+)/(?P<mode>off|zc_auto)$")
THRESHOLD_RE = re.compile(r"^threshold_(?P<num>[0-9]+)(?P<unit>[kKmM]?)(?:_load)?$")


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(2)


def parse_threshold_config(config: str) -> int | None:
    match = THRESHOLD_RE.match(config)
    if not match:
        return None
    value = int(match.group("num"))
    unit = match.group("unit").lower()
    if unit == "k":
        return value * 1024
    if unit == "m":
        return value * 1024 * 1024
    return value


def as_number(row: dict[str, Any], key: str, line_no: int) -> float:
    value = row.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        fail(f"line {line_no}: {key} must be numeric")
    return float(value)


def as_non_negative_int(row: dict[str, Any], key: str, line_no: int) -> int:
    value = row.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        fail(f"line {line_no}: {key} must be a non-negative integer")
    return value


def median(values: list[float]) -> float:
    if not values:
        fail("internal error: median of empty list")
    return float(statistics.median(values))


def ratio(numerator: float, denominator: float) -> float:
    if denominator <= 0.0:
        return 0.0
    return round(numerator / denominator, 6)


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
            match = MODE_RE.match(variant)
            if not match:
                fail(f"line {line_no}: variant must end in /off or /zc_auto: {variant}")
            for key in TIMING_KEYS:
                as_number(row, key, line_no)
            if row["iterations"] <= 0 or row["total_ns"] < 0 or row["ns_per_iter"] < 0:
                fail(f"line {line_no}: timing fields must be non-negative and iterations > 0")
            for key in COUNTER_KEYS:
                as_non_negative_int(row, key, line_no)
            if "requests_per_sec" in row:
                as_number(row, "requests_per_sec", line_no)
                as_non_negative_int(row, "errors", line_no)
                as_non_negative_int(row, "connections", line_no)
                as_non_negative_int(row, "duration_s", line_no)
            if "rep" in row:
                as_non_negative_int(row, "rep", line_no)
            threshold = row.get("send_zc_threshold")
            if threshold is None:
                threshold = parse_threshold_config(config)
            if threshold is not None:
                if not isinstance(threshold, int) or threshold <= 0:
                    fail(f"line {line_no}: send_zc_threshold must be a positive integer")
                row["send_zc_threshold"] = threshold
            rows.append(row)
    if not rows:
        fail(f"no NDJSON rows found in {path}")
    return rows


def mode_and_pair_key(variant: str) -> tuple[str, str]:
    match = MODE_RE.match(variant)
    assert match is not None
    return match.group("mode"), match.group("stem")


def classify_pair(pair_key: str) -> dict[str, Any]:
    parts = pair_key.split("/")
    load = bool(parts and parts[0] == "load")
    tls = bool(parts and parts[0] == "tls")
    body_label = parts[-1] if parts else pair_key
    family = "/".join(parts[:-1]) if len(parts) > 1 else pair_key
    return {"family": family, "body": body_label, "load": load, "tls": tls}


def summarize_bucket(rows: list[dict[str, Any]]) -> dict[str, Any]:
    ns = [float(row["ns_per_iter"]) for row in rows]
    out: dict[str, Any] = {
        "samples": len(rows),
        "median_ns_per_iter": median(ns),
        "best_ns_per_iter": min(ns),
        "median_iterations": median([float(row["iterations"]) for row in rows]),
    }
    if all("requests_per_sec" in row for row in rows):
        rps = [float(row["requests_per_sec"]) for row in rows]
        out["median_requests_per_sec"] = median(rps)
        out["best_requests_per_sec"] = max(rps)
        out["errors"] = sum(int(row["errors"]) for row in rows)
        out["connections"] = int(median([float(row["connections"]) for row in rows]))
        out["duration_s"] = int(median([float(row["duration_s"]) for row in rows]))
    counters = {key: sum(int(row[key]) for row in rows) for key in COUNTER_KEYS}
    out["counters"] = counters
    out["copied_notification_rate"] = ratio(
        float(counters["zc_copied_notifications"]), float(counters["zc_notifications"])
    )
    out["fallback_rate_per_attempt"] = ratio(
        float(counters["zc_fallback_regular_send"]), float(counters["zc_attempts"])
    )
    return out


def pair_status(zc: dict[str, Any]) -> str:
    counters = zc["counters"]
    if counters["zc_tls_bypass"] > 0:
        return "tls_bypass"
    if counters["zc_attempts"] == 0:
        return "no_zc_attempts"
    if counters["zc_errors_enomem"] > 0 or counters["zc_errors_other"] > 0:
        return "zc_errors"
    if counters["zc_fallback_regular_send"] > 0:
        return "submit_fallback"
    if zc["copied_notification_rate"] > 0.90 and counters["zc_notifications"] > 0:
        return "mostly_copied"
    return "ok"


def summarize(rows: list[dict[str, Any]], raw_path: Path, expected_configs: set[str]) -> dict[str, Any]:
    configs = sorted({str(row["config"]) for row in rows})
    missing_configs = sorted(expected_configs - set(configs))
    if missing_configs:
        fail("missing expected configs: " + ", ".join(missing_configs))

    by_config_variant: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        by_config_variant[(row["config"], row["variant"])].append(row)

    variants = []
    for (config, variant), bucket in sorted(by_config_variant.items()):
        mode, pair_key = mode_and_pair_key(variant)
        entry = {
            "config": config,
            "variant": variant,
            "mode": mode,
            "pair_key": pair_key,
            "send_zc_threshold": bucket[0].get("send_zc_threshold"),
        }
        entry.update(classify_pair(pair_key))
        entry.update(summarize_bucket(bucket))
        variants.append(entry)

    by_config_pair: dict[tuple[str, str], dict[str, dict[str, Any]]] = defaultdict(dict)
    for entry in variants:
        by_config_pair[(entry["config"], entry["pair_key"])][entry["mode"]] = entry

    pairs = []
    missing_pairs = []
    for (config, pair_key), modes in sorted(by_config_pair.items()):
        if "off" not in modes or "zc_auto" not in modes:
            missing_pairs.append(f"{config}/{pair_key}")
            continue
        off = modes["off"]
        zc = modes["zc_auto"]
        pair = {
            "config": config,
            "pair_key": pair_key,
            "send_zc_threshold": zc.get("send_zc_threshold"),
            **classify_pair(pair_key),
            "off_median_ns_per_iter": off["median_ns_per_iter"],
            "zc_median_ns_per_iter": zc["median_ns_per_iter"],
            "ns_speedup": ratio(off["median_ns_per_iter"], zc["median_ns_per_iter"]),
            "off_best_ns_per_iter": off["best_ns_per_iter"],
            "zc_best_ns_per_iter": zc["best_ns_per_iter"],
            "best_ns_speedup": ratio(off["best_ns_per_iter"], zc["best_ns_per_iter"]),
            "status": pair_status(zc),
            "zc_counters": zc["counters"],
            "copied_notification_rate": zc["copied_notification_rate"],
            "fallback_rate_per_attempt": zc["fallback_rate_per_attempt"],
        }
        if "median_requests_per_sec" in off and "median_requests_per_sec" in zc:
            pair["off_median_requests_per_sec"] = off["median_requests_per_sec"]
            pair["zc_median_requests_per_sec"] = zc["median_requests_per_sec"]
            pair["rps_speedup"] = ratio(zc["median_requests_per_sec"], off["median_requests_per_sec"])
            pair["errors"] = int(off.get("errors", 0)) + int(zc.get("errors", 0))
            pair["connections"] = zc.get("connections")
            pair["duration_s"] = zc.get("duration_s")
        pairs.append(pair)

    if missing_pairs:
        fail("missing off/zc_auto pair rows: " + ", ".join(missing_pairs))

    threshold_rollups = []
    for config in configs:
        config_pairs = [pair for pair in pairs if pair["config"] == config]
        if not config_pairs:
            continue
        ok_pairs = [pair for pair in config_pairs if pair["status"] == "ok"]
        load_pairs = [pair for pair in ok_pairs if pair["load"]]
        non_load_pairs = [pair for pair in ok_pairs if not pair["load"]]
        load_rps_speedups = [pair["rps_speedup"] for pair in load_pairs if "rps_speedup" in pair]
        statuses = {pair["status"] for pair in config_pairs}
        threshold_rollups.append(
            {
                "config": config,
                "send_zc_threshold": config_pairs[0].get("send_zc_threshold"),
                "pairs": len(config_pairs),
                "ok_pairs": len(ok_pairs),
                "median_ok_ns_speedup": median([pair["ns_speedup"] for pair in ok_pairs]) if ok_pairs else 0.0,
                "best_ok_ns_speedup": max([pair["ns_speedup"] for pair in ok_pairs], default=0.0),
                "median_ok_load_rps_speedup": median(load_rps_speedups) if load_rps_speedups else 0.0,
                "median_ok_non_load_ns_speedup": median([pair["ns_speedup"] for pair in non_load_pairs])
                if non_load_pairs
                else 0.0,
                "statuses": dict(
                    sorted(
                        (status, sum(1 for pair in config_pairs if pair["status"] == status))
                        for status in statuses
                    )
                ),
            }
        )

    return {
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "raw_ndjson": str(raw_path),
        "configs": configs,
        "rows": len(rows),
        "variants": variants,
        "pairs": pairs,
        "threshold_rollups": threshold_rollups,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw_ndjson", type=Path)
    parser.add_argument("--output", "-o", type=Path)
    parser.add_argument(
        "--expected-config",
        action="append",
        default=[],
        help="config name that must appear; may be repeated",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = read_rows(args.raw_ndjson)
    doc = summarize(rows, args.raw_ndjson, set(args.expected_config))
    text = json.dumps(doc, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
