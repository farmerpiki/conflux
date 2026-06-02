#!/usr/bin/env bash
# Capture SEND_ZC threshold evidence without changing HTTP send-path defaults.
#
# Optional env:
#   SEND_ZC_PRESET           CMake preset; default perf-clang-libcxx
#   SEND_ZC_THRESHOLDS       space-separated thresholds; default "4096 16384 65536"
#   SEND_ZC_ITERATIONS       measured iterations per request-at-a-time rep; default 1000
#   SEND_ZC_WARMUP           warmup iterations per request-at-a-time rep; default 20% of SEND_ZC_ITERATIONS
#   SEND_ZC_REPS             repeated benchmark launches per threshold/config; default 5
#   SEND_ZC_LOAD             run concurrent keep-alive load sweep; default 1
#   SEND_ZC_CONNECTIONS      concurrent load connections; default 64
#   SEND_ZC_DURATION         concurrent load duration seconds per rep; default 2
#   SEND_ZC_ARTIFACT_DIR     output dir; default /tmp/<repo>/send-zc-threshold-evidence/<stamp>
set -euo pipefail

usage() {
	cat >&2 <<'USAGE'
usage: scripts/send_zc_threshold_evidence.sh

Runs:
  1. configure the selected perf preset
  2. build conflux_send_zc_bench
  3. run repeated threshold sweeps into NDJSON
  4. summarize off vs zc_auto pairs, load RPS, and SEND_ZC counters into JSON
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
		printf 'threshold_%dm' "$((value / 1048576))"
	elif (( value % 1024 == 0 )); then
		printf 'threshold_%dk' "$((value / 1024))"
	else
		printf 'threshold_%d' "$value"
	fi
}

append_with_metadata() {
	local src=$1 rep=$2 threshold=$3 mode=$4 out=$5
	jq -c --argjson rep "$rep" --argjson threshold "$threshold" --arg mode "$mode" \
		'. + {rep: $rep, send_zc_threshold: $threshold, sweep_mode: $mode}' "$src" >> "$out"
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

PRESET="${SEND_ZC_PRESET:-perf-clang-libcxx}"
python3 scripts/cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET" >/dev/null
THRESHOLDS="${SEND_ZC_THRESHOLDS:-4096 16384 65536}"
ITERATIONS="${SEND_ZC_ITERATIONS:-1000}"
WARMUP="${SEND_ZC_WARMUP:-$(( ITERATIONS / 5 ))}"
(( WARMUP < 1 )) && WARMUP=1
REPS="${SEND_ZC_REPS:-5}"
LOAD="${SEND_ZC_LOAD:-1}"
CONNECTIONS="${SEND_ZC_CONNECTIONS:-64}"
DURATION="${SEND_ZC_DURATION:-2}"
RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
ARTIFACT_DIR="${SEND_ZC_ARTIFACT_DIR:-/tmp/$(basename "$REPO_ROOT")/send-zc-threshold-evidence/$RUN_STAMP}"

require_positive_int SEND_ZC_ITERATIONS "$ITERATIONS"
require_positive_int SEND_ZC_WARMUP "$WARMUP"
require_positive_int SEND_ZC_REPS "$REPS"
require_positive_int SEND_ZC_CONNECTIONS "$CONNECTIONS"
require_positive_int SEND_ZC_DURATION "$DURATION"
if [[ "$LOAD" != 0 && "$LOAD" != 1 ]]; then
	printf 'SEND_ZC_LOAD must be 0 or 1: %s\n' "$LOAD" >&2
	exit 2
fi

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
raw_ndjson="$ARTIFACT_DIR/send_zc_threshold.raw.ndjson"
summary_json="$ARTIFACT_DIR/send_zc_threshold.summary.json"
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
if ! cmake --build "$BUILD_DIR" --target conflux_send_zc_bench \
	> "$build_log" 2>&1; then
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
		printf 'running SEND_ZC threshold %s request rep %s/%s\n' "$threshold" "$rep" "$REPS"
		rep_ndjson="$ARTIFACT_DIR/send_zc.${label}.rep${rep}.tmp.ndjson"
		"$bench_bin" \
			--send-zc-threshold "$threshold" \
			--config-name "$label" \
			--iterations "$ITERATIONS" \
			--warmup "$WARMUP" \
			--json > "$rep_ndjson"
		append_with_metadata "$rep_ndjson" "$rep" "$threshold" request "$raw_ndjson"
		rm -f "$rep_ndjson"
	done

	if [[ "$LOAD" == 1 ]]; then
		load_label="${label}_load"
		summary_args+=(--expected-config "$load_label")
		for rep in $(seq 1 "$REPS"); do
			printf 'running SEND_ZC threshold %s load rep %s/%s\n' "$threshold" "$rep" "$REPS"
			rep_ndjson="$ARTIFACT_DIR/send_zc.${load_label}.rep${rep}.tmp.ndjson"
			"$bench_bin" \
				--concurrent \
				--connections "$CONNECTIONS" \
				--duration "$DURATION" \
				--send-zc-threshold "$threshold" \
				--config-name "$load_label" \
				--json > "$rep_ndjson"
			append_with_metadata "$rep_ndjson" "$rep" "$threshold" load "$raw_ndjson"
			rm -f "$rep_ndjson"
		done
	fi
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
	"$THRESHOLDS" \
	"$ITERATIONS" \
	"$WARMUP" \
	"$REPS" \
	"$LOAD" \
	"$CONNECTIONS" \
	"$DURATION" <<'PY'
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
    thresholds,
    iterations,
    warmup,
    reps,
    load,
    connections,
    duration,
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
    "thresholds": [int(item) for item in thresholds.split()],
    "iterations": int(iterations),
    "warmup": int(warmup),
    "reps": int(reps),
    "load": load == "1",
    "connections": int(connections),
    "duration_s": int(duration),
    "commit": git_value("rev-parse", "HEAD"),
    "branch": git_value("rev-parse", "--abbrev-ref", "HEAD"),
}
pathlib.Path(out_path).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

printf 'SEND_ZC threshold evidence written to %s\n' "$ARTIFACT_DIR"
printf 'summary: %s\n' "$summary_json"
