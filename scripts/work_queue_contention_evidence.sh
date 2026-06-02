#!/usr/bin/env bash
# Capture WorkPool queue contention evidence without changing scheduler semantics.
#
# Optional env:
#   WORK_QUEUE_PRESET          CMake preset; default perf-clang-libcxx
#   WORK_QUEUE_THREADS         producer/worker count passed to the benchmark; default nproc
#   WORK_QUEUE_ITERATIONS      measured iterations per rep; default 5000
#   WORK_QUEUE_WARMUP          warmup iterations per rep; default 20% of WORK_QUEUE_ITERATIONS
#   WORK_QUEUE_WORK            CPU work units per redistribution child job; default 2048
#   WORK_QUEUE_REPS            repeated benchmark launches; default 5
#   WORK_QUEUE_ARTIFACT_DIR    output dir; default /tmp/<repo>/work-queue-evidence/<stamp>
set -euo pipefail

usage() {
	cat >&2 <<'USAGE'
usage: scripts/work_queue_contention_evidence.sh

Runs:
  1. configure with -DCONFLUX_WORK_QUEUE_STATS=ON
  2. build conflux_workpool_queue_mode_compare_bench
  3. run repeated --json benchmark reps into NDJSON
  4. summarize queue contention/fairness counters into JSON
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

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
	usage
	exit 0
fi
if [[ $# -ne 0 ]]; then
	usage
	exit 2
fi

for tool in cmake nproc python3; do
	require_tool "$tool"
done

REPO_ROOT="${SOURCE_DIR:-$(git rev-parse --show-toplevel 2>/dev/null || script_repo_root)}"
cd "$REPO_ROOT"

PRESET="${WORK_QUEUE_PRESET:-perf-clang-libcxx}"
python3 scripts/cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET" >/dev/null
THREADS="${WORK_QUEUE_THREADS:-$(nproc)}"
ITERATIONS="${WORK_QUEUE_ITERATIONS:-5000}"
WARMUP="${WORK_QUEUE_WARMUP:-$(( ITERATIONS / 5 ))}"
(( WARMUP < 1 )) && WARMUP=1
WORK_UNITS="${WORK_QUEUE_WORK:-2048}"
REPS="${WORK_QUEUE_REPS:-5}"
RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
ARTIFACT_DIR="${WORK_QUEUE_ARTIFACT_DIR:-/tmp/$(basename "$REPO_ROOT")/work-queue-evidence/$RUN_STAMP}"

require_positive_int WORK_QUEUE_THREADS "$THREADS"
require_positive_int WORK_QUEUE_ITERATIONS "$ITERATIONS"
require_positive_int WORK_QUEUE_WARMUP "$WARMUP"
require_positive_int WORK_QUEUE_WORK "$WORK_UNITS"
require_positive_int WORK_QUEUE_REPS "$REPS"

mkdir -p "$ARTIFACT_DIR"
configure_log="$ARTIFACT_DIR/configure.log"
build_log="$ARTIFACT_DIR/build.log"
raw_ndjson="$ARTIFACT_DIR/workpool_queue_mode_compare.raw.ndjson"
summary_json="$ARTIFACT_DIR/workpool_queue_mode_compare.summary.json"
manifest_json="$ARTIFACT_DIR/manifest.json"
config_name="threads_${THREADS}"

printf 'configuring preset %s with CONFLUX_WORK_QUEUE_STATS=ON\n' "$PRESET"
cmake --preset "$PRESET" -DCONFLUX_WORK_QUEUE_STATS=ON > "$configure_log" 2>&1
BUILD_DIR="$(sed -n 's/^-- Build files have been written to: //p' "$configure_log" | tail -1)"
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
	printf 'configure failed; log=%s\n' "$configure_log" >&2
	tail -40 "$configure_log" >&2
	exit 2
fi

printf 'building workpool queue mode comparison benchmark\n'
if ! cmake --build "$BUILD_DIR" --target conflux_workpool_queue_mode_compare_bench \
	> "$build_log" 2>&1; then
	printf 'build failed; log=%s\n' "$build_log" >&2
	tail -40 "$build_log" >&2
	exit 2
fi

bench_bin="$BUILD_DIR/benchmarks/conflux_workpool_queue_mode_compare_bench"
: > "$raw_ndjson"
for rep in $(seq 1 "$REPS"); do
	printf 'running workpool queue mode comparison bench rep %s/%s\n' "$rep" "$REPS"
	"$bench_bin" \
		--threads "$THREADS" \
		--iterations "$ITERATIONS" \
		--warmup "$WARMUP" \
		--work "$WORK_UNITS" \
		--config-name "$config_name" \
		--json >> "$raw_ndjson"
done

python3 scripts/work_queue_contention_summary.py "$raw_ndjson" --output "$summary_json"

python3 - \
	"$manifest_json" \
	"$PRESET" \
	"$BUILD_DIR" \
	"$ARTIFACT_DIR" \
	"$raw_ndjson" \
	"$summary_json" \
	"$THREADS" \
	"$ITERATIONS" \
	"$WARMUP" \
	"$WORK_UNITS" \
	"$REPS" <<'PY'
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
    threads,
    iterations,
    warmup,
    work_units,
    reps,
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
    "threads": int(threads),
    "iterations": int(iterations),
    "warmup": int(warmup),
    "work_units": int(work_units),
    "reps": int(reps),
    "commit": git_value("rev-parse", "HEAD"),
    "branch": git_value("rev-parse", "--abbrev-ref", "HEAD"),
}
pathlib.Path(out_path).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

printf 'WorkPool queue contention evidence written to %s\n' "$ARTIFACT_DIR"
printf 'summary: %s\n' "$summary_json"
