#!/usr/bin/env bash
# Capture NVMe storage-read evidence for the IOPOLL/read_fixed gate.
#
# This is a host evidence wrapper. It refuses to run without an explicit path so
# artifacts can identify the storage device under test. The benchmark itself
# rejects non-NVMe paths unless STORAGE_READ_ALLOW_NON_NVME=1 is set for smoke.
#
# Required env:
#   STORAGE_READ_PATH          file path on the target NVMe filesystem
#
# Optional env:
#   STORAGE_READ_PRESET        CMake preset; default perf-clang-libcxx
#   STORAGE_READ_REPS          repeated benchmark launches per config; default 6
#   STORAGE_READ_MODES         benchmark mode; default all
#   STORAGE_READ_MATRIX        comma list depth:chunk:label; default documented gate matrix
#   STORAGE_READ_ALLOW_NON_NVME set 1 for smoke on non-NVMe path; default 0
#   STORAGE_READ_KEEP_FILE     set 1 to retain seed file; default 1
#   STORAGE_READ_ARTIFACT_DIR  output dir; default /tmp/<repo>/storage-read-evidence/<stamp>
set -euo pipefail

usage() {
	cat >&2 <<'USAGE'
usage: STORAGE_READ_PATH=/mnt/nvme/conflux-storage-read.bin scripts/storage_read_evidence.sh

Runs repeated storage_read rows and stores raw NDJSON plus host/build metadata.
Default matrix:
  depth_1_4k, depth_8_16k, depth_32_64k, depth_128_1m

The wrapper is evidence scaffolding, not a claim by itself. Publish raw rows,
manifest, configure/build logs, and hardware counters captured externally.
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

for tool in cmake python3; do
	require_tool "$tool"
done

REPO_ROOT="${SOURCE_DIR:-$(git rev-parse --show-toplevel 2>/dev/null || script_repo_root)}"
cd "$REPO_ROOT"

PATH_UNDER_TEST="${STORAGE_READ_PATH:-}"
if [[ -z "$PATH_UNDER_TEST" ]]; then
	printf 'STORAGE_READ_PATH is required\n' >&2
	usage
	exit 2
fi

PRESET="${STORAGE_READ_PRESET:-perf-clang-libcxx}"
REPS="${STORAGE_READ_REPS:-6}"
MODES="${STORAGE_READ_MODES:-all}"
MATRIX="${STORAGE_READ_MATRIX:-depth_1_4k:1:4096,depth_8_16k:8:16384,depth_32_64k:32:65536,depth_128_1m:128:1048576}"
ALLOW_NON_NVME="${STORAGE_READ_ALLOW_NON_NVME:-0}"
KEEP_FILE="${STORAGE_READ_KEEP_FILE:-1}"
RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
ARTIFACT_DIR="${STORAGE_READ_ARTIFACT_DIR:-/tmp/$(basename "$REPO_ROOT")/storage-read-evidence/$RUN_STAMP}"

require_positive_int STORAGE_READ_REPS "$REPS"
if [[ "$ALLOW_NON_NVME" != 0 && "$ALLOW_NON_NVME" != 1 ]]; then
	printf 'STORAGE_READ_ALLOW_NON_NVME must be 0 or 1: %s\n' "$ALLOW_NON_NVME" >&2
	exit 2
fi
if [[ "$KEEP_FILE" != 0 && "$KEEP_FILE" != 1 ]]; then
	printf 'STORAGE_READ_KEEP_FILE must be 0 or 1: %s\n' "$KEEP_FILE" >&2
	exit 2
fi

mkdir -p "$ARTIFACT_DIR"
configure_log="$ARTIFACT_DIR/configure.log"
build_log="$ARTIFACT_DIR/build.log"
raw_ndjson="$ARTIFACT_DIR/storage_read.raw.ndjson"
manifest_json="$ARTIFACT_DIR/manifest.json"
env_txt="$ARTIFACT_DIR/env.txt"

printf 'configuring preset %s\n' "$PRESET"
cmake --preset "$PRESET" > "$configure_log" 2>&1
BUILD_DIR="$(sed -n 's/^-- Build files have been written to: //p' "$configure_log" | tail -1)"
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
	printf 'configure failed; log=%s\n' "$configure_log" >&2
	tail -40 "$configure_log" >&2
	exit 2
fi

printf 'building storage_read benchmark\n'
if ! cmake --build "$BUILD_DIR" --target conflux_storage_read_bench > "$build_log" 2>&1; then
	printf 'build failed; log=%s\n' "$build_log" >&2
	tail -40 "$build_log" >&2
	exit 2
fi

python3 scripts/bench_env_summary.py > "$env_txt" || true
bench_bin="$BUILD_DIR/benchmarks/conflux_storage_read_bench"
: > "$raw_ndjson"

IFS=',' read -r -a matrix_rows <<< "$MATRIX"
for row in "${matrix_rows[@]}"; do
	IFS=':' read -r label depth chunk <<< "$row"
	if [[ -z "${label:-}" || -z "${depth:-}" || -z "${chunk:-}" ]]; then
		printf 'invalid STORAGE_READ_MATRIX entry: %s\n' "$row" >&2
		exit 2
	fi
	require_positive_int matrix_depth "$depth"
	require_positive_int matrix_chunk "$chunk"
	for rep in $(seq 1 "$REPS"); do
		printf 'running storage_read %s rep %s/%s\n' "$label" "$rep" "$REPS"
		rep_ndjson="$ARTIFACT_DIR/storage_read.${label}.rep${rep}.tmp.ndjson"
		cmd=(
			"$bench_bin"
			--path "$PATH_UNDER_TEST"
			--depth "$depth"
			--chunk "$chunk"
			--mode "$MODES"
			--config-name "$label"
			--json
		)
		if [[ "$ALLOW_NON_NVME" == 1 ]]; then
			cmd+=(--allow-non-nvme)
		fi
		if [[ "$KEEP_FILE" == 1 ]]; then
			cmd+=(--keep-file)
		fi
		"${cmd[@]}" > "$rep_ndjson"
		python3 - "$rep_ndjson" "$raw_ndjson" "$rep" "$depth" "$chunk" "$MODES" <<'PY'
import json
import pathlib
import sys
src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
rep = int(sys.argv[3])
depth = int(sys.argv[4])
chunk = int(sys.argv[5])
modes = sys.argv[6]
with dst.open("a", encoding="utf-8") as out:
    for line in src.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        row.update({
            "rep": rep,
            "requested_depth": depth,
            "requested_chunk": chunk,
            "requested_modes": modes,
            "evidence_role": "nvme_iopoll_gate",
        })
        out.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
PY
		rm -f "$rep_ndjson"
	done
done

python3 - \
	"$manifest_json" \
	"$PRESET" \
	"$BUILD_DIR" \
	"$ARTIFACT_DIR" \
	"$raw_ndjson" \
	"$env_txt" \
	"$PATH_UNDER_TEST" \
	"$MATRIX" \
	"$MODES" \
	"$REPS" \
	"$ALLOW_NON_NVME" \
	"$KEEP_FILE" <<'PY'
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
    env_txt,
    storage_path,
    matrix,
    modes,
    reps,
    allow_non_nvme,
    keep_file,
) = sys.argv[1:]

def git_value(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return "unknown"

manifest = {
    "artifact_kind": "storage_read_evidence",
    "preset": preset,
    "build_dir": build_dir,
    "artifact_dir": artifact_dir,
    "raw_ndjson": raw_ndjson,
    "env_txt": env_txt,
    "storage_path": storage_path,
    "matrix": matrix,
    "modes": modes,
    "reps": int(reps),
    "allow_non_nvme": allow_non_nvme == "1",
    "keep_file": keep_file == "1",
    "label": "live-kernel-sanity",
    "requires_external_perf_stat": True,
    "recommended_perf_stat": "cycles,instructions,cache-misses,dTLB-load-misses,cs",
    "commit": git_value("rev-parse", "HEAD"),
    "branch": git_value("rev-parse", "--abbrev-ref", "HEAD"),
}
pathlib.Path(out_path).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

printf 'storage_read evidence written to %s\n' "$ARTIFACT_DIR"
printf 'raw rows: %s\n' "$raw_ndjson"
