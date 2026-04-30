#!/usr/bin/env bash
# bench_record.sh — run a curated set of benchmarks for the current checkout
# and write results into the conflux_bench Postgres database.
#
# Each invocation creates a new row in `runs` (with commit_sha, branch, dirty,
# host, build_preset, compiler) and inserts one row per benchmark variant into
# `results` linked by run_id (FK).
#
# Re-runnable: just run again — produces a new run row with current commit.
#
# Usage:
#   PGURI=postgres://postgres@localhost/conflux_bench scripts/bench_record.sh [run-name]
#
# Defaults: PGURI=postgres://postgres@localhost/conflux_bench, build preset =
# release-clang-libcxx (built first if needed).

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
case "$PRESET" in
  *gcc*) COMPILER="g++" ;;
esac

# Resolve binaryDir from the preset (handles both /tmp/... and ${sourceDir}/build/... layouts).
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

psql_q() { psql "$PGURI" -At -q -F$'\t' -c "$1"; }

RUN_ID="$(psql "$PGURI" -At -q -c "
INSERT INTO runs (name, commit_sha, branch, dirty, host, build_preset, compiler)
VALUES ('$NAME', '$COMMIT', '$BRANCH', $DIRTY, '$HOST', '$PRESET', '$COMPILER')
RETURNING id;")"

echo "run_id=$RUN_ID  commit=$COMMIT  branch=$BRANCH  dirty=$DIRTY"

insert_row() {
  local bench="$1" variant="$2" iters="$3" total_ns="$4" ns_per_iter="$5"
  local extra="${6-}"
  [[ -z "$extra" ]] && extra='{}'
  psql "$PGURI" -At -q -c "
    INSERT INTO results (run_id, benchmark, variant, iterations, total_ns, ns_per_iter, extra)
    VALUES ($RUN_ID, '$bench', '$variant', $iters, $total_ns, $ns_per_iter, '$extra'::jsonb);" >/dev/null
}

# tcp_increment: style,iterations,total_ns,ns_per_iter
echo "+ tcp_increment_coro"
"$BENCHDIR/conflux_tcp_increment_coro_bench" --iterations 200 --warmup 50 --csv 2>/dev/null \
  | tail -n +2 | while IFS=, read -r style iters total ns_pi; do
    insert_row tcp_increment_coro "$style" "$iters" "$total" "$ns_pi"
  done

# tcp_parallel: config,flags,ring_entries,parallel,iters_per_conn,total_iters,total_ns,ns_per_iter,throughput_iter_per_s
echo "+ tcp_parallel_coro"
"$BENCHDIR/conflux_tcp_parallel_coro_bench" --iterations 200 --warmup 50 --parallel 1,2,4,8 --csv 2>/dev/null \
  | tail -n +2 | while IFS=, read -r config flags ring par ipc total_iters total_ns ns_pi tput; do
    extra=$(printf '{"parallel":%s,"throughput_iter_per_s":%s,"ring_entries":%s}' "$par" "$tput" "$ring")
    insert_row tcp_parallel_coro "P=$par" "$total_iters" "$total_ns" "$ns_pi" "$extra"
  done

# file_copy: style,runs,avg_ns,best_ns,avg_mib_per_s,best_mib_per_s
echo "+ file_copy_coro"
"$BENCHDIR/conflux_file_copy_coro_bench" --size-mib 64 --runs 5 --csv 2>/dev/null \
  | grep -E '^(style|callback|coroutine)' | tail -n +2 | while IFS=, read -r style runs avg_ns best_ns avg_mibs best_mibs; do
    extra=$(printf '{"avg_mib_per_s":%s,"best_mib_per_s":%s,"best_ns":%s}' "$avg_mibs" "$best_mibs" "$best_ns")
    insert_row file_copy_coro "$style" "$runs" "$avg_ns" "$avg_ns" "$extra"
  done

echo "done. run_id=$RUN_ID"
