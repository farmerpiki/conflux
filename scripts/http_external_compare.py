#!/usr/bin/env python3
"""Run same-host HTTP comparison scenarios and write raw + summary JSON.

The harness intentionally keeps servers external to the repository. Each target
provides its own command and base URL; scenarios are driven with wrk so all
libraries see the same client, payload, connection count, thread count, and
run order. It records all raw repetitions and summarizes external best-of-N vs
Conflux worst-of-N without hiding medians/min/max.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import random
import re
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REQ_RE = re.compile(r"Requests/sec:\s+([0-9.]+)")
TRANSFER_RE = re.compile(r"Transfer/sec:\s+([0-9.]+)([KMGT]?B)")
LAT_RE = re.compile(r"Latency\s+([0-9.]+)(us|ms|s)\s+([0-9.]+)(us|ms|s)\s+([0-9.]+)(us|ms|s)")
PCT_RE = re.compile(r"\s+([0-9.]+)%\s+([0-9.]+)(us|ms|s)")


def ns(value: str, unit: str) -> int:
    v = float(value)
    if unit == "us":
        return int(v * 1_000)
    if unit == "ms":
        return int(v * 1_000_000)
    if unit == "s":
        return int(v * 1_000_000_000)
    raise ValueError(unit)


def bytes_per_sec(value: str, unit: str) -> float:
    scale = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3, "TB": 1024**4}[unit]
    return float(value) * scale


def read_text(path: str) -> str:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return "unknown"


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def metadata(spec_path: Path) -> dict[str, Any]:
    return {
        "spec_path": str(spec_path),
        "hostname": platform.node(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "cpu_model": cpu_model(),
        "cpu_count": os.cpu_count(),
        "governor": read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "pinned_cpus": os.environ.get("HTTP_EXTERNAL_PIN_CPUS", ""),
        "wrk": shutil_which("wrk"),
    }


def shutil_which(name: str) -> str:
    from shutil import which

    return which(name) or ""


@dataclass
class TargetProc:
    spec: dict[str, Any]
    proc: subprocess.Popen[str] | None = None

    def start(self) -> None:
        cmd = self.spec.get("cmd")
        if not cmd:
            raise RuntimeError(f"target {self.spec['name']} has no cmd")
        log_path = Path(self.spec["_artifact_dir"]) / f"server_{self.spec['name']}.log"
        log = log_path.open("w", encoding="utf-8")
        self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, text=True)
        self.wait_ready()

    def wait_ready(self) -> None:
        deadline = time.monotonic() + float(self.spec.get("ready_timeout_s", 20))
        ready_url = self.spec["base_url"].rstrip("/") + self.spec.get("ready_path", "/")
        while time.monotonic() < deadline:
            if self.proc and self.proc.poll() is not None:
                raise RuntimeError(f"target {self.spec['name']} exited during startup")
            try:
                with urllib.request.urlopen(ready_url, timeout=1) as resp:
                    if 200 <= resp.status < 500:
                        return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError(f"target {self.spec['name']} not ready at {ready_url}")

    def stop(self) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def parse_wrk(out: str) -> dict[str, Any]:
    req = REQ_RE.search(out)
    lat = LAT_RE.search(out)
    transfer = TRANSFER_RE.search(out)
    percentiles: dict[str, int] = {}
    for pct, value, unit in PCT_RE.findall(out):
        if pct in {"50.000", "75.000", "90.000", "99.000", "99.900"}:
            percentiles[f"p{pct.replace('.', '_')}_ns"] = ns(value, unit)
    parsed: dict[str, Any] = {"raw_output": out}
    if req:
        parsed["requests_per_sec"] = float(req.group(1))
    if lat:
        parsed["latency_avg_ns"] = ns(lat.group(1), lat.group(2))
        parsed["latency_stdev_ns"] = ns(lat.group(3), lat.group(4))
        parsed["latency_max_ns"] = ns(lat.group(5), lat.group(6))
    if transfer:
        parsed["transfer_bytes_per_sec"] = bytes_per_sec(transfer.group(1), transfer.group(2))
    parsed.update(percentiles)
    return parsed


def wrk_command(target: dict[str, Any], scenario: dict[str, Any], body_file: Path | None) -> list[str]:
    cmd = [
        "wrk",
        "-t",
        str(scenario.get("threads", 4)),
        "-c",
        str(scenario.get("connections", 64)),
        "-d",
        str(scenario.get("duration", "10s")),
        "--latency",
    ]
    for header in scenario.get("headers", []):
        cmd += ["-H", header]
    if scenario.get("method", "GET") != "GET" or body_file is not None:
        script = (
            "wrk.method = '" + scenario.get("method", "GET") + "'\n"
            + (f"wrk.body = assert(io.open('{body_file}', 'rb')):read('*a')\n" if body_file else "")
            + "wrk.headers['Content-Type'] = 'application/octet-stream'\n"
        )
        script_path = body_file.with_suffix(".lua") if body_file else Path(tempfile.mkstemp(suffix=".lua")[1])
        script_path.write_text(script, encoding="utf-8")
        cmd += ["-s", str(script_path)]
    cmd.append(target["base_url"].rstrip("/") + scenario["path"])
    pin = os.environ.get("HTTP_EXTERNAL_PIN_CPUS")
    if pin:
        cmd = ["taskset", "-c", pin] + cmd
    return cmd


def run_wrk(target: dict[str, Any], scenario: dict[str, Any], artifact_dir: Path, rep: int) -> dict[str, Any]:
    body_file: Path | None = None
    body_bytes = int(scenario.get("body_bytes", 0) or 0)
    if body_bytes > 0:
        body_file = artifact_dir / f"body_{scenario['name']}_{body_bytes}.bin"
        if not body_file.exists():
            body_file.write_bytes(bytes((i % 251 for i in range(body_bytes))))
    cmd = wrk_command(target, scenario, body_file)
    started = time.monotonic_ns()
    cp = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    ended = time.monotonic_ns()
    parsed = parse_wrk(cp.stdout)
    parsed.update(
        {
            "target": target["name"],
            "target_kind": target.get("kind", "external"),
            "scenario": scenario["name"],
            "rep": rep,
            "cmd": cmd,
            "returncode": cp.returncode,
            "elapsed_ns": ended - started,
        }
    )
    return parsed


def summarize(rows: list[dict[str, Any]], reps: int) -> dict[str, Any]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault((row["scenario"], row["target"]), []).append(row)
    summaries: list[dict[str, Any]] = []
    for (scenario, target), group in sorted(grouped.items()):
        rps = sorted(float(r.get("requests_per_sec", 0.0)) for r in group)
        p99 = sorted(int(r.get("p99_000_ns", 0)) for r in group if "p99_000_ns" in r)
        summaries.append(
            {
                "scenario": scenario,
                "target": target,
                "target_kind": group[0].get("target_kind"),
                "reps": len(group),
                "requests_per_sec_min": rps[0] if rps else 0.0,
                "requests_per_sec_median": rps[len(rps) // 2] if rps else 0.0,
                "requests_per_sec_max": rps[-1] if rps else 0.0,
                "p99_ns_min": p99[0] if p99 else None,
                "p99_ns_median": p99[len(p99) // 2] if p99 else None,
                "p99_ns_max": p99[-1] if p99 else None,
                "selection_rule": "conflux worst-of-N, external best-of-N",
                "selected_requests_per_sec": rps[0] if group[0].get("target_kind") == "conflux" and rps else (rps[-1] if rps else 0.0),
            }
        )
    by_scenario: dict[str, list[dict[str, Any]]] = {}
    for item in summaries:
        by_scenario.setdefault(item["scenario"], []).append(item)
    comparisons: list[dict[str, Any]] = []
    for scenario, items in by_scenario.items():
        conflux = [i for i in items if i.get("target_kind") == "conflux"]
        externals = [i for i in items if i.get("target_kind") != "conflux"]
        if not conflux:
            continue
        cf = conflux[0]
        for ext in externals:
            base = ext["selected_requests_per_sec"]
            mine = cf["selected_requests_per_sec"]
            comparisons.append(
                {
                    "scenario": scenario,
                    "conflux_target": cf["target"],
                    "external_target": ext["target"],
                    "conflux_worst_rps": mine,
                    "external_best_rps": base,
                    "rps_ratio_conflux_worst_over_external_best": (mine / base) if base else None,
                }
            )
    return {"reps_requested": reps, "summaries": summaries, "comparisons": comparisons}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("spec", type=Path, help="HTTP external comparison spec JSON")
    parser.add_argument("--artifact-dir", type=Path)
    parser.add_argument("--shuffle", action="store_true", help="shuffle target/scenario order per rep")
    args = parser.parse_args()

    spec = json.loads(args.spec.read_text())
    artifact_dir = args.artifact_dir or Path(spec.get("artifact_dir", "/tmp/conflux-http-external"))
    artifact_dir.mkdir(parents=True, exist_ok=True)
    if not shutil_which("wrk"):
        raise SystemExit("wrk not found in PATH")

    meta = metadata(args.spec)
    (artifact_dir / "manifest.json").write_text(json.dumps(meta, indent=2, sort_keys=True), encoding="utf-8")
    reps = int(spec.get("reps", 6))
    rows: list[dict[str, Any]] = []
    raw_path = artifact_dir / "raw.ndjson"
    targets = spec["targets"]
    scenarios = spec["scenarios"]

    with raw_path.open("w", encoding="utf-8") as raw:
        for target in targets:
            target = dict(target)
            target["_artifact_dir"] = str(artifact_dir)
            proc = TargetProc(target)
            try:
                proc.start()
                time.sleep(float(spec.get("warmup_seconds", 0)))
                plan = [(scenario, rep) for scenario in scenarios for rep in range(1, reps + 1)]
                if args.shuffle:
                    random.shuffle(plan)
                for scenario, rep in plan:
                    row = run_wrk(target, scenario, artifact_dir, rep)
                    raw.write(json.dumps(row, sort_keys=True) + "\n")
                    raw.flush()
                    rows.append(row)
                    print(
                        f"{target['name']} {scenario['name']} rep={rep} rps={row.get('requests_per_sec', 0):.1f}",
                        file=sys.stderr,
                    )
            finally:
                proc.stop()

    summary = summarize(rows, reps)
    (artifact_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    print(artifact_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
