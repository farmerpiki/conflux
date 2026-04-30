#!/usr/bin/env bash
# bench_record.sh — record benchmark results into the conflux_bench database.
#
# One `runs` row per (commit, benchmark, config_name). All variants produced
# by that single (bench, config) invocation share that run_id via the
# results.run_id FK. Re-runnable; each invocation produces fresh datapoints.
#
# Build dirs under /tmp are wiped at exit so repeated cross-branch sweeps do
# not exhaust the tmpfs.
#
# Usage:
#   PGURI=postgres://postgres@localhost/conflux_bench scripts/bench_record.sh [run-name]
#
# Env knobs:
#   PGURI         postgres URI (default: postgres://postgres@localhost/conflux_bench)
#   BENCH_PRESET  cmake preset (default: release-clang-libcxx)
#   KEEP_BUILD=1  skip /tmp build-dir cleanup
#   ONLY_BENCH    if set, run only that benchmark (e.g. "tcp_parallel_coro")

set -euo pipefail

PGURI="${PGURI:-postgres://postgres@localhost/conflux_bench}"
PRESET="${BENCH_PRESET:-release-clang-libcxx}"
NAME="${1:-manual}"

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

COMMIT="$(git rev-parse HEAD)"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
DIRTY=false
if ! git diff --quiet || ! git diff --cached --quiet; then DIRTY=true; fi

HOST="$(hostname)"
COMPILER="clang++"
case "$PRESET" in *gcc*) COMPILER="g++" ;; esac

CONFIGURE_LOG=$(cmake --preset "$PRESET" 2>&1)
BUILD_DIR=$(printf '%s\n' "$CONFIGURE_LOG" | sed -n 's/^-- Build files have been written to: //p' | tail -1)
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
  echo "configure failed; preset=$PRESET" >&2
  printf '%s\n' "$CONFIGURE_LOG" | tail -20 >&2
  exit 2
fi

cmake --build "$BUILD_DIR" --target \
  conflux_tcp_increment_coro_bench \
  conflux_tcp_parallel_coro_bench \
  conflux_file_copy_coro_bench \
  >/dev/null

BENCHDIR="$BUILD_DIR/benchmarks"
CONFIGS_DIR="${BENCH_CONFIGS_DIR:-$REPO_ROOT/benchmarks/configs}"
# fall back to the top-level configs/ dir if the benchmarks copy is absent
[[ -d "$CONFIGS_DIR" ]] || CONFIGS_DIR="$REPO_ROOT/configs"

cleanup() {
  if [[ "${KEEP_BUILD:-0}" != "1" && "$BUILD_DIR" == /tmp/* ]]; then
    rm -rf "$BUILD_DIR"
    echo "cleaned $BUILD_DIR"
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# DB helpers
# ---------------------------------------------------------------------------
sql_escape() { printf '%s' "$1" | sed "s/'/''/g"; }

new_run() {
  local bench="$1" config="$2" extra="$3"
  psql "$PGURI" -At -q -c "
    INSERT INTO runs
      (name, commit_sha, branch, dirty, host, build_preset, compiler,
       benchmark, config_name, config_extra)
    VALUES
      ('$(sql_escape "$NAME")', '$COMMIT', '$BRANCH', $DIRTY,
       '$HOST', '$PRESET', '$COMPILER',
       '$(sql_escape "$bench")', '$(sql_escape "$config")', '$(sql_escape "$extra")'::jsonb)
    RETURNING id;"
}

insert_row() {
  local run_id="$1" bench="$2" variant="$3" iters="$4" total_ns="$5" ns_pi="$6"
  local extra="${7-}"; [[ -z "$extra" ]] && extra='{}'
  psql "$PGURI" -At -q -c "
    INSERT INTO results (run_id, benchmark, variant, iterations, total_ns, ns_per_iter, extra)
    VALUES ($run_id, '$(sql_escape "$bench")', '$(sql_escape "$variant")',
            $iters, $total_ns, $ns_pi, '$(sql_escape "$extra")'::jsonb);" >/dev/null
}

want() { [[ -z "${ONLY_BENCH:-}" || "${ONLY_BENCH}" == "$1" ]]; }

# ---------------------------------------------------------------------------
# tcp_increment_coro — single config (no ring knobs exposed by the bench)
# variants: callback, coroutine
# ---------------------------------------------------------------------------
if want tcp_increment_coro; then
  CFG="default"
  RID=$(new_run tcp_increment_coro "$CFG" "{}")
  echo "+ tcp_increment_coro [$CFG] run_id=$RID"
  "$BENCHDIR/conflux_tcp_increment_coro_bench" --iterations 200 --warmup 50 --csv 2>/dev/null \
    | tail -n +2 | while IFS=, read -r style iters total ns_pi; do
      insert_row "$RID" tcp_increment_coro "$style" "$iters" "$total" "$ns_pi"
    done
fi

# ---------------------------------------------------------------------------
# tcp_parallel_coro — ring config matrix
# variants: P=1, P=2, P=4, P=8
# ---------------------------------------------------------------------------
if want tcp_parallel_coro; then
  shopt -s nullglob
  cfg_paths=("$CONFIGS_DIR"/*.json)
  shopt -u nullglob
  if (( ${#cfg_paths[@]} == 0 )); then
    echo "skip tcp_parallel_coro: no configs in $CONFIGS_DIR"
  fi
  for path in "${cfg_paths[@]}"; do
    cfgfile="$(basename "$path" .json)"
    extra=$(cat "$path")
    RID=$(new_run tcp_parallel_coro "$cfgfile" "$extra")
    echo "+ tcp_parallel_coro [$cfgfile] run_id=$RID"
    "$BENCHDIR/conflux_tcp_parallel_coro_bench" \
        --iterations 200 --warmup 50 --parallel 1,2,4,8 --config "$path" --csv 2>/dev/null \
      | tail -n +2 | while IFS=, read -r config flags ring par ipc total_iters total_ns ns_pi tput; do
        ex=$(printf '{"flags":"%s","ring_entries":%s,"throughput_iter_per_s":%s}' "$flags" "$ring" "$tput")
        insert_row "$RID" tcp_parallel_coro "P=$par" "$total_iters" "$total_ns" "$ns_pi" "$ex"
      done
  done
fi

# ---------------------------------------------------------------------------
# file_copy_coro — size/chunk matrix
# variants: callback, coroutine
# ---------------------------------------------------------------------------
if want file_copy_coro; then
  # cfg_name  size_mib  chunk_kib  runs
  for spec in "small 4 16 5" "medium 64 64 5" "large 256 256 3"; do
    read -r cfg size chunk runs <<< "$spec"
    extra=$(printf '{"size_mib":%s,"chunk_kib":%s,"runs":%s}' "$size" "$chunk" "$runs")
    RID=$(new_run file_copy_coro "$cfg" "$extra")
    echo "+ file_copy_coro [$cfg size=${size}MiB chunk=${chunk}KiB] run_id=$RID"
    "$BENCHDIR/conflux_file_copy_coro_bench" \
        --size-mib "$size" --chunk-kib "$chunk" --runs "$runs" --csv 2>/dev/null \
      | grep -E '^(callback|coroutine),' \
      | while IFS=, read -r style runs_v avg_ns best_ns avg_mibs best_mibs; do
        ex=$(printf '{"avg_mib_per_s":%s,"best_mib_per_s":%s,"best_ns":%s}' "$avg_mibs" "$best_mibs" "$best_ns")
        insert_row "$RID" file_copy_coro "$style" "$runs_v" "$avg_ns" "$avg_ns" "$ex"
      done
  done
fi

echo "done."
