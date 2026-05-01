#!/usr/bin/env bash
# bench_record.sh — auto-discovering benchmark recorder for the conflux_bench database.
#
# Discovery: every conflux_*bench binary in the build dir is queried with --bench-info.
# Binaries that implement --bench-info are recorded; others are skipped with a warning.
# To add a new bench: implement --bench-info in the binary and add the target to the
# conflux_record_benches CMake meta-target in benchmarks/CMakeLists.txt.
#
# Bench binary contract:
#   --bench-info   print JSON descriptor (see below) and exit 0
#   --csv          output standard CSV: variant,iterations,total_ns,ns_per_iter
#
# --bench-info JSON:
#   {
#     "name":    "logical_bench_name",          # used as runs.benchmark
#     "parser":  "standard|strip1|tcp_parallel|file_copy",
#     "configs": [
#       { "name": "cfg", "extra": {}, "args": ["--iterations","N",...] }
#     ]
#   }
#   Parsers:
#     standard    — CSV variant,iterations,total_ns,ns_per_iter; uses record_with_reps
#     strip1      — same but first CSV column (config name) is stripped before recording
#     tcp_parallel — custom parser; configs read from $CONFIGS_DIR/*.json (args/extra ignored)
#     file_copy   — custom parser; configs come from --bench-info JSON
#
# Usage:
#   PGURI=postgres://postgres@localhost/conflux_bench scripts/bench_record.sh [run-name]
#
# Env knobs:
#   PGURI              postgres URI (default: postgres://postgres@localhost/conflux_bench)
#   BENCH_PRESET       cmake preset (default: release-clang-libcxx)
#   KEEP_BUILD=1       skip /tmp build-dir cleanup
#   ONLY_BENCH         if set, run only that logical bench name (e.g. "task_creation")
#   BENCH_PIN_CPUS     cpuset for taskset (e.g. "0-3"); wraps every bench launch
#   BENCH_REPS         per-metric replications consumed by record_with_reps (default: 5)
#   MACHINE_ID         overrides machine identity (default: /etc/machine-id)
#   WAIVER_REASON      free-form waiver text, persisted to runs.waiver_reason
#
# Pre-bench checklist (not enforced; operator responsibility):
#   - CPU governor = performance
#   - swap off
#   - isolcpus recommended for BENCH_PIN_CPUS set
#   - no concurrent heavy processes
#
# The script records /sys/.../scaling_governor into runs.metadata.governor per run
# (read-only; setting governor requires root and is brittle in CI).

set -euo pipefail

PGURI="${PGURI:-postgres://postgres@localhost/conflux_bench}"
PRESET="${BENCH_PRESET:-release-clang-libcxx}"
NAME="${1:-manual}"
BENCH_REPS="${BENCH_REPS:-5}"
MACHINE_ID="${MACHINE_ID:-$(cat /etc/machine-id 2>/dev/null || hostname)}"
WAIVER_REASON="${WAIVER_REASON:-}"

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

COMMIT="$(git rev-parse HEAD)"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
DIRTY=false
if ! git diff --quiet || ! git diff --cached --quiet; then DIRTY=true; fi

HOST="$(hostname)"
COMPILER="clang++"
case "$PRESET" in *gcc*) COMPILER="g++" ;; esac

# ---------------------------------------------------------------------------
# System metadata (read-only)
# ---------------------------------------------------------------------------
cpu_model()  { grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //' || echo 'unknown'; }
cpu_cores()  { nproc 2>/dev/null || echo 1; }
cpu_smt()    { [[ -f /sys/devices/system/cpu/smt/active ]] && cat /sys/devices/system/cpu/smt/active || echo 'unknown'; }
kernel_ver() { uname -r; }
libc_ver()   { ldd --version 2>/dev/null | head -1 || echo 'unknown'; }
governor()   { cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'unknown'; }
compiler_version() {
  case "$COMPILER" in
    clang++) clang++ --version 2>/dev/null | head -1 || echo 'unknown' ;;
    g++)     g++ --version 2>/dev/null | head -1 || echo 'unknown' ;;
    *)       echo 'unknown' ;;
  esac
}

SYS_CPU=$(cpu_model)
SYS_CORES=$(cpu_cores)
SYS_SMT=$(cpu_smt)
SYS_KERNEL=$(kernel_ver)
SYS_LIBC=$(libc_ver)
SYS_GOVERNOR=$(governor)
SYS_COMPILER_VER=$(compiler_version)
SYS_PINNED_CPUS="${BENCH_PIN_CPUS:-}"

METADATA=$(printf '{
  "cpu": "%s",
  "cores": %s,
  "smt": "%s",
  "kernel": "%s",
  "libc": "%s",
  "governor": "%s",
  "compiler_version": "%s",
  "pinned_cpus": "%s",
  "reps": %s
}' \
  "$SYS_CPU" "$SYS_CORES" "$SYS_SMT" "$SYS_KERNEL" \
  "$SYS_LIBC" "$SYS_GOVERNOR" "$SYS_COMPILER_VER" \
  "$SYS_PINNED_CPUS" "$BENCH_REPS")

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
CONFIGURE_LOG=$(cmake --preset "$PRESET" 2>&1)
BUILD_DIR=$(printf '%s\n' "$CONFIGURE_LOG" | sed -n 's/^-- Build files have been written to: //p' | tail -1)
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
  echo "configure failed; preset=$PRESET" >&2
  printf '%s\n' "$CONFIGURE_LOG" | tail -20 >&2
  exit 2
fi

cmake --build "$BUILD_DIR" --target conflux_record_benches -- -j"$(nproc)" >/dev/null

BENCHDIR="$BUILD_DIR/benchmarks"
CONFIGS_DIR="${BENCH_CONFIGS_DIR:-$REPO_ROOT/benchmarks/configs}"
[[ -d "$CONFIGS_DIR" ]] || CONFIGS_DIR="$REPO_ROOT/configs"

cleanup() {
  if [[ "${KEEP_BUILD:-0}" != "1" && "$BUILD_DIR" == /tmp/* ]]; then
    rm -rf "$BUILD_DIR"
    echo "cleaned $BUILD_DIR"
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Bench launcher — wraps with taskset if BENCH_PIN_CPUS is set
# ---------------------------------------------------------------------------
run_bench() {
  if [[ -n "$SYS_PINNED_CPUS" ]]; then
    taskset -c "$SYS_PINNED_CPUS" "$@"
  else
    "$@"
  fi
}

# run_bench_strip1: drops the first comma-separated column (config name prefix)
run_bench_strip1() { run_bench "$@" | cut -d, -f2-; }

# ---------------------------------------------------------------------------
# DB helpers
# ---------------------------------------------------------------------------
sql_escape() { printf '%s' "$1" | sed "s/'/''/g"; }

new_run() {
  local bench="$1" config="$2" extra="$3"
  local waiver_sql="NULL"
  [[ -n "$WAIVER_REASON" ]] && waiver_sql="'$(sql_escape "$WAIVER_REASON")'"
  psql "$PGURI" -At -q -c "
    INSERT INTO runs
      (name, commit_sha, branch, dirty, host, build_preset, compiler,
       benchmark, config_name, config_extra, machine_id, metadata, waiver_reason)
    VALUES
      ('$(sql_escape "$NAME")', '$COMMIT', '$BRANCH', $DIRTY,
       '$HOST', '$PRESET', '$COMPILER',
       '$(sql_escape "$bench")', '$(sql_escape "$config")', '$(sql_escape "$extra")'::jsonb,
       '$(sql_escape "$MACHINE_ID")', '$(sql_escape "$METADATA")'::jsonb,
       $waiver_sql)
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

# record_with_reps: runs bench $BENCH_REPS times, inserts raw rows + summary row.
# Args: run_id bench_name <bench_args_to_produce_csv>...
# Expects CSV with header: variant,iterations,total_ns,ns_per_iter
record_with_reps() {
  local run_id="$1" bench="$2"; shift 2
  local reps="${BENCH_REPS:-5}"
  local tmpf
  tmpf=$(mktemp /tmp/bench_reps_XXXXXX.csv)
  trap 'rm -f "$tmpf"' RETURN

  local i; for i in $(seq 1 "$reps"); do
    "$@" 2>/dev/null | tail -n +2 >> "$tmpf"
  done

  while IFS=, read -r variant iters total ns_pi _rest; do
    psql "$PGURI" -At -q -c "
      INSERT INTO results
        (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
         metric, value, unit, sample_count)
      VALUES
        ($run_id, '$(sql_escape "$bench")', '$(sql_escape "$variant")',
         $iters, $total, $ns_pi,
         'ns_per_iter', $ns_pi, 'ns', 1);" >/dev/null
  done < "$tmpf"

  psql "$PGURI" -At -q -c "
    WITH raw AS (
      SELECT variant,
             PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS med,
             PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS p50,
             PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY ns_per_iter) AS p99,
             COUNT(*) AS n,
             AVG(iterations) AS avg_iters
      FROM results
      WHERE run_id = $run_id AND benchmark = '$(sql_escape "$bench")'
      GROUP BY variant
    ),
    mad_raw AS (
      SELECT r.variant, raw.med,
             PERCENTILE_CONT(0.5) WITHIN GROUP (
               ORDER BY ABS(r.ns_per_iter - raw.med)) AS mad,
             raw.p50, raw.p99, raw.avg_iters, raw.n
      FROM results r
      JOIN raw ON raw.variant = r.variant
      WHERE r.run_id = $run_id AND r.benchmark = '$(sql_escape "$bench")'
      GROUP BY r.variant, raw.med, raw.p50, raw.p99, raw.avg_iters, raw.n
    )
    INSERT INTO results
      (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
       metric, value, unit, sample_count, median, mad, p50, p99, extra)
    SELECT $run_id, '$(sql_escape "$bench")', variant,
           avg_iters::bigint, 0, med,
           'ns_per_iter', med, 'ns', n::bigint,
           med, mad, p50, p99,
           '{\"kind\":\"summary\"}'::jsonb
    FROM mad_raw;" >/dev/null
}

want() { [[ -z "${ONLY_BENCH:-}" || "${ONLY_BENCH}" == "$1" ]]; }

# ---------------------------------------------------------------------------
# Custom parsers (for benches with non-standard CSV formats)
# ---------------------------------------------------------------------------

# tcp_parallel_coro — complex CSV; configs from CONFIGS_DIR/*.json
run_tcp_parallel() {
  local binary="$1" bench="$2"
  shopt -s nullglob
  local cfg_paths=("$CONFIGS_DIR"/*.json)
  shopt -u nullglob
  if (( ${#cfg_paths[@]} == 0 )); then
    echo "skip $bench: no configs in $CONFIGS_DIR"
    return
  fi
  for path in "${cfg_paths[@]}"; do
    local cfgfile extra RID
    cfgfile="$(basename "$path" .json)"
    extra=$(cat "$path")
    RID=$(new_run "$bench" "$cfgfile" "$extra")
    echo "+ $bench [$cfgfile] run_id=$RID"
    run_bench "$binary" \
        --iterations 200 --warmup 50 --parallel 1,2,4,8 --config "$path" --csv 2>/dev/null \
      | tail -n +2 | while IFS=, read -r config flags ring par ipc total_iters total_ns ns_pi tput; do
        local ex
        ex=$(printf '{"flags":"%s","ring_entries":%s,"throughput_iter_per_s":%s}' "$flags" "$ring" "$tput")
        insert_row "$RID" "$bench" "P=$par" "$total_iters" "$total_ns" "$ns_pi" "$ex"
      done
  done
}

# file_copy_coro — non-standard CSV; configs from --bench-info JSON
run_file_copy() {
  local binary="$1" bench="$2" cfg_name="$3" extra="$4"
  shift 4
  local args=("$@")
  local RID
  RID=$(new_run "$bench" "$cfg_name" "$extra")
  echo "+ $bench [$cfg_name] run_id=$RID"
  run_bench "$binary" "${args[@]}" --csv 2>/dev/null \
    | grep -E '^(callback|coroutine),' \
    | while IFS=, read -r style runs_v avg_ns best_ns avg_mibs best_mibs; do
        local ex
        ex=$(printf '{"avg_mib_per_s":%s,"best_mib_per_s":%s,"best_ns":%s}' "$avg_mibs" "$best_mibs" "$best_ns")
        insert_row "$RID" "$bench" "$style" "$runs_v" "$avg_ns" "$avg_ns" "$ex"
      done
}

# ---------------------------------------------------------------------------
# Auto-discovery loop
# ---------------------------------------------------------------------------
for binary in "$BENCHDIR"/conflux_*bench; do
  [[ -x "$binary" ]] || continue

  info=$("$binary" --bench-info 2>/dev/null) || {
    echo "skip $(basename "$binary"): no --bench-info support" >&2
    continue
  }

  bench_name=$(jq -r .name <<< "$info")
  parser=$(jq -r .parser <<< "$info")

  want "$bench_name" || continue

  if [[ "$parser" == "tcp_parallel" ]]; then
    run_tcp_parallel "$binary" "$bench_name"
    continue
  fi

  # Iterate configs from --bench-info JSON
  while IFS= read -r cfg_json; do
    cfg_name=$(jq -r .name <<< "$cfg_json")
    extra=$(jq -c .extra <<< "$cfg_json")
    mapfile -t args < <(jq -r '.args[]' <<< "$cfg_json")

    if [[ "$parser" == "file_copy" ]]; then
      run_file_copy "$binary" "$bench_name" "$cfg_name" "$extra" "${args[@]}"
      continue
    fi

    RID=$(new_run "$bench_name" "$cfg_name" "$extra")
    echo "+ $bench_name [$cfg_name] run_id=$RID"

    case "$parser" in
      standard)
        record_with_reps "$RID" "$bench_name" \
          run_bench "$binary" "${args[@]}" --csv
        ;;
      strip1)
        record_with_reps "$RID" "$bench_name" \
          run_bench_strip1 "$binary" "${args[@]}" --csv
        ;;
      *)
        echo "unknown parser '$parser' for $bench_name, skipping" >&2
        ;;
    esac
  done < <(jq -c '.configs[]' <<< "$info")
done

echo "done."
