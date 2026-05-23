#!/usr/bin/env bash
# Capture SEND_ZC evidence through a non-loopback host path.
#
# This is a live-kernel/NIC-candidate harness, not a default recorder row. It
# refuses loopback by default, records off-vs-zc_auto pairs with server-side
# SEND_ZC counters and client-side request rate in the same NDJSON row, and
# leaves final NIC proof to the operator's route/NIC counter evidence.
#
# Required env:
#   SEND_ZC_NIC_HOST         non-loopback IPv4 address clients should connect to
#
# Optional env:
#   SEND_ZC_PRESET           CMake preset; default perf-clang-libcxx
#   SEND_ZC_THRESHOLDS       space-separated thresholds; default "4096 16384 65536"
#   SEND_ZC_REPS             repeated benchmark launches per threshold; default 5
#   SEND_ZC_CONNECTIONS      concurrent keep-alive clients; default 64
#   SEND_ZC_DURATION         load duration seconds per rep; default 5
#   SEND_ZC_NIC_PATH         request path; default generated 64 KiB/1 MiB matrix
#   SEND_ZC_ALLOW_LOOPBACK   pass --allow-loopback-remote for smoke only; default 0
#   SEND_ZC_ARTIFACT_DIR     output dir; default /tmp/<repo>/send-zc-nic-evidence/<stamp>
set -euo pipefail

usage() {
	cat >&2 <<'USAGE'
usage: SEND_ZC_NIC_HOST=<non-loopback-ipv4> scripts/send_zc_nic_evidence.sh

Runs:
  1. configure selected perf preset with experimental SEND_ZC enabled
  2. build conflux_send_zc_bench
  3. run repeated --nic-concurrent off-vs-zc_auto threshold sweeps
  4. summarize request rate, SEND_ZC counters, copied notifications, and fallbacks

This harness rejects 127.0.0.0/8 and 0.0.0.0 unless SEND_ZC_ALLOW_LOOPBACK=1.
Use host routing/NIC counters outside this script to prove packets crossed a NIC.
USAGE
}

require_tool() {
	command -v "$1" >/dev/null 2>&1 || {
		printf 'required tool not found in PATH: %s\n' "$1" >&2
		exit 2
	}
}

script_repo_root() {
	local script_dir
	script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	cd "$script_dir/.." && pwd
}

require_positive_int() {
	local name=$1 value=$2
	if ! [[ "$value" =~ ^[1-9][0-9]*$ ]]; then
		printf '%s must be a positive integer: %s\n' "$name" "$value" >&2
		exit 2
	fi
}

threshold_label() {
	local value=$1
	if (( value % 1048576 == 0 )); then
		printf 'threshold_%dm_nic' "$((value / 1048576))"
	elif (( value % 1024 == 0 )); then
		printf 'threshold_%dk_nic' "$((value / 1024))"
	else
		printf 'threshold_%d_nic' "$value"
	fi
}

append_with_metadata() {
	local src=$1 rep=$2 threshold=$3 out=$4
	jq -c --argjson rep "$rep" --argjson threshold "$threshold" \
		'. + {rep: $rep, send_zc_threshold: $threshold, sweep_mode: "nic"}' "$src" >> "$out"
}

is_loopback_ipv4() {
	python3 - "$1" <<'PY'
import ipaddress
import sys
try:
    addr = ipaddress.ip_address(sys.argv[1])
except ValueError:
    raise SystemExit(1)
raise SystemExit(0 if addr.version == 4 and (addr.is_loopback or addr.is_unspecified) else 1)
PY
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
	usage
	exit 0
fi
if [[ $# -ne 0 ]]; then
	usage
	exit 2
fi

for tool in cmake jq nproc python3; do
	require_tool "$tool"
done

REPO_ROOT="${SOURCE_DIR:-$(git rev-parse --show-toplevel 2>/dev/null || script_repo_root)}"
cd "$REPO_ROOT"

HOST="${SEND_ZC_NIC_HOST:-}"
if [[ -z "$HOST" ]]; then
	printf 'SEND_ZC_NIC_HOST is required\n' >&2
	usage
	exit 2
fi
ALLOW_LOOPBACK="${SEND_ZC_ALLOW_LOOPBACK:-0}"
if [[ "$ALLOW_LOOPBACK" != 0 && "$ALLOW_LOOPBACK" != 1 ]]; then
	printf 'SEND_ZC_ALLOW_LOOPBACK must be 0 or 1: %s\n' "$ALLOW_LOOPBACK" >&2
	exit 2
fi
if [[ "$ALLOW_LOOPBACK" == 0 ]] && is_loopback_ipv4 "$HOST"; then
	printf 'SEND_ZC_NIC_HOST must be non-loopback for evidence: %s\n' "$HOST" >&2
	printf 'Set SEND_ZC_ALLOW_LOOPBACK=1 only for smoke runs.\n' >&2
	exit 2
fi

PRESET="${SEND_ZC_PRESET:-perf-clang-libcxx}"
THRESHOLDS="${SEND_ZC_THRESHOLDS:-4096 16384 65536}"
REPS="${SEND_ZC_REPS:-5}"
CONNECTIONS="${SEND_ZC_CONNECTIONS:-64}"
DURATION="${SEND_ZC_DURATION:-5}"
PATH_ARG="${SEND_ZC_NIC_PATH:-}"
RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
ARTIFACT_DIR="${SEND_ZC_ARTIFACT_DIR:-/tmp/$(basename "$REPO_ROOT")/send-zc-nic-evidence/$RUN_STAMP}"

require_positive_int SEND_ZC_REPS "$REPS"
require_positive_int SEND_ZC_CONNECTIONS "$CONNECTIONS"
require_positive_int SEND_ZC_DURATION "$DURATION"
read -r -a threshold_values <<< "$THRESHOLDS"
if ((${#threshold_values[@]} == 0)); then
	printf 'SEND_ZC_THRESHOLDS must name at least one threshold\n' >&2
	exit 2
fi
for threshold in "${threshold_values[@]}"; do
	require_positive_int SEND_ZC_THRESHOLDS "$threshold"
done

mkdir -p "$ARTIFACT_DIR"
configure_log="$ARTIFACT_DIR/configure.log"
build_log="$ARTIFACT_DIR/build.log"
raw_ndjson="$ARTIFACT_DIR/send_zc_nic.raw.ndjson"
summary_json="$ARTIFACT_DIR/send_zc_nic.summary.json"
manifest_json="$ARTIFACT_DIR/manifest.json"

printf 'configuring preset %s with CONFLUX_ENABLE_EXPERIMENTAL=ON\n' "$PRESET"
cmake --preset "$PRESET" -DCONFLUX_ENABLE_EXPERIMENTAL=ON > "$configure_log" 2>&1
BUILD_DIR="$(sed -n 's/^-- Build files have been written to: //p' "$configure_log" | tail -1)"
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
	printf 'configure failed; log=%s\n' "$configure_log" >&2
	tail -40 "$configure_log" >&2
	exit 2
fi

printf 'building SEND_ZC benchmark\n'
if ! cmake --build "$BUILD_DIR" --target conflux_send_zc_bench > "$build_log" 2>&1; then
	printf 'build failed; log=%s\n' "$build_log" >&2
	tail -40 "$build_log" >&2
	exit 2
fi

bench_bin="$BUILD_DIR/benchmarks/conflux_send_zc_bench"
: > "$raw_ndjson"
summary_args=()
for threshold in "${threshold_values[@]}"; do
	label="$(threshold_label "$threshold")"
	summary_args+=(--expected-config "$label")
	for rep in $(seq 1 "$REPS"); do
		printf 'running SEND_ZC NIC threshold %s rep %s/%s via %s\n' "$threshold" "$rep" "$REPS" "$HOST"
		rep_ndjson="$ARTIFACT_DIR/send_zc.${label}.rep${rep}.tmp.ndjson"
		cmd=(
			"$bench_bin"
			--nic-concurrent
			--host "$HOST"
			--connections "$CONNECTIONS"
			--duration "$DURATION"
			--send-zc-threshold "$threshold"
			--config-name "$label"
			--json
		)
		if [[ -n "$PATH_ARG" ]]; then
			cmd+=(--path "$PATH_ARG")
		fi
		if [[ "$ALLOW_LOOPBACK" == 1 ]]; then
			cmd+=(--allow-loopback-remote)
		fi
		"${cmd[@]}" > "$rep_ndjson"
		append_with_metadata "$rep_ndjson" "$rep" "$threshold" "$raw_ndjson"
		rm -f "$rep_ndjson"
	done
done

python3 scripts/send_zc_threshold_summary.py "$raw_ndjson" \
	--output "$summary_json" \
	"${summary_args[@]}"

python3 - \
	"$manifest_json" \
	"$PRESET" \
	"$BUILD_DIR" \
	"$ARTIFACT_DIR" \
	"$raw_ndjson" \
	"$summary_json" \
	"$HOST" \
	"$THRESHOLDS" \
	"$REPS" \
	"$CONNECTIONS" \
	"$DURATION" \
	"$ALLOW_LOOPBACK" <<'PY'
import json
import pathlib
import subprocess
import sys

(
    out_path,
    preset,
    build_dir,
    artifact_dir,
    raw_ndjson,
    summary_json,
    host,
    thresholds,
    reps,
    connections,
    duration,
    allow_loopback,
) = sys.argv[1:]


def git_value(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return "unknown"

manifest = {
    "preset": preset,
    "build_dir": build_dir,
    "artifact_dir": artifact_dir,
    "raw_ndjson": raw_ndjson,
    "summary_json": summary_json,
    "host": host,
    "thresholds": [int(item) for item in thresholds.split()],
    "reps": int(reps),
    "connections": int(connections),
    "duration_s": int(duration),
    "allow_loopback": allow_loopback == "1",
    "transport": "non_loopback_nic_candidate",
    "requires_external_route_or_nic_counter_proof": True,
    "commit": git_value("rev-parse", "HEAD"),
    "branch": git_value("rev-parse", "--abbrev-ref", "HEAD"),
}
pathlib.Path(out_path).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

printf 'SEND_ZC NIC evidence written to %s\n' "$ARTIFACT_DIR"
printf 'summary: %s\n' "$summary_json"
