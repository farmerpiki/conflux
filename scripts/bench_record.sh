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
#   --json         output NDJSON: one {"config":"","variant":"","iterations":N,"total_ns":N,"ns_per_iter":X} per line
#                  optional fields are preserved in results.extra for standard-parser raw rows
#
# --bench-info JSON:
#   {
#     "name":    "logical_bench_name",          # used as runs.benchmark
#     "parser":  "standard|file_copy",
#     "configs": [
#       { "name": "cfg", "extra": {}, "args": ["--iterations","N",...], "reps": 2 }
#     ]
#   }
#   Parsers:
#     standard    — NDJSON variant,iterations,total_ns,ns_per_iter; uses record_with_reps
#     file_copy   — custom parser; configs come from --bench-info JSON
#
# Usage:
#   PGURI=postgres://postgres@localhost/conflux_bench scripts/bench_record.sh [run-name]
#
# Env knobs:
#   PGURI              postgres URI (default: postgres://postgres@localhost/conflux_bench)
#   BENCH_PRESET       space-separated cmake preset(s) (default: perf-clang-libcxx perf-gcc-stdcxx)
#                      each preset is built, run, and deleted before the next one starts
#   KEEP_BUILD=1       skip /tmp build-dir cleanup
#   ONLY_BENCH         if set, run only that logical bench name (e.g. "task_creation")
#   BENCH_PIN_CPUS     cpuset for taskset (e.g. "0-3"); wraps every bench launch
#   BENCH_REPS         default per-metric replications consumed by record_with_reps (default: 5)
#                      per-config --bench-info .reps overrides this for expensive benches
#   BENCH_ARTIFACT_DIR directory for manifest, bench-info, and raw NDJSON artifacts
#                      (default: /tmp/<repo>/bench-artifacts/<timestamp>-<run-name>)
#   BENCH_LOAD_FACTOR  abort when 1-min load exceeds nproc * factor (default: 1.5)
#   BENCH_SETTLE_AFTER_BUILD_SEC  post-build settle delay (default: 20)
#   BENCH_SETTLE_BETWEEN_SEC      inter-candidate settle delay in compare modes (default: 2)
#   BENCH_ITERATIONS_FROM_RUN_ID
#                      if set, read per-benchmark/config iteration counts from this
#                      prior run_id in the database and override launch args.
#                      If unset, the script falls back to the latest summary rows
#                      already present in the database.
#   ALLOW_NON_PERF_BENCH_PRESET=1
#                      allow non-perf presets for explicit experiments. The normal
#                      recording path requires perf-* presets, RelWithDebInfo,
#                      benchmark-only builds, no LTO, and no sanitizers.
#   CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON
#                      allow sanitizer-instrumented local benchmark debugging.
#                      Never use such runs for performance conclusions.
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

require_tool() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "required tool not found in PATH: $1" >&2
    exit 2
  }
}

for tool in cmake jq psql; do
  require_tool "$tool"
done
if [[ -n "${BENCH_PIN_CPUS:-}" ]]; then
  require_tool taskset
fi

PGURI="${PGURI:-postgres://postgres@localhost/conflux_bench}"
export PG_CONNINFO="${PG_CONNINFO:-host=localhost dbname=conflux_bench user=postgres}"
BENCH_PRESETS="${BENCH_PRESET:-perf-clang-libcxx perf-gcc-stdcxx}"
BENCH_REPS="${BENCH_REPS:-5}"
MACHINE_ID="${MACHINE_ID:-$(cat /etc/machine-id 2>/dev/null || hostname)}"
WAIVER_REASON="${WAIVER_REASON:-}"
BENCH_ITERATIONS_FROM_RUN_ID="${BENCH_ITERATIONS_FROM_RUN_ID:-}"
ALLOW_NON_PERF_BENCH_PRESET="${ALLOW_NON_PERF_BENCH_PRESET:-0}"
CONFLUX_ALLOW_SANITIZED_BENCHMARKS="${CONFLUX_ALLOW_SANITIZED_BENCHMARKS:-OFF}"
BENCH_LOAD_FACTOR="${BENCH_LOAD_FACTOR:-1.5}"
BENCH_SETTLE_AFTER_BUILD_SEC="${BENCH_SETTLE_AFTER_BUILD_SEC:-20}"
BENCH_SETTLE_BETWEEN_SEC="${BENCH_SETTLE_BETWEEN_SEC:-2}"

COMPARE_MODE=false
COMPARE_BINS_MODE=false
COMPARE_PRESETS=()
COMPARE_BINS_ARGS=()
if [[ "${1:-}" == "--compare" ]]; then
  COMPARE_MODE=true
  shift
  COMPARE_PRESETS=("$@")
  NAME="compare"
elif [[ "${1:-}" == "--compare-bins" ]]; then
  COMPARE_BINS_MODE=true
  shift
  COMPARE_BINS_ARGS=("$@")
  NAME="compare-bins"
else
  NAME="${1:-manual}"
fi

script_repo_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "$script_dir/.." && pwd
}

REPO_ROOT="${SOURCE_DIR:-$(git rev-parse --show-toplevel 2>/dev/null || script_repo_root)}"
cd "$REPO_ROOT"

RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
safe_run_name="$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9_.-' '_')"
BENCH_ARTIFACT_DIR="${BENCH_ARTIFACT_DIR:-/tmp/$(basename "$REPO_ROOT")/bench-artifacts/${RUN_STAMP}-${safe_run_name}}"
mkdir -p "$BENCH_ARTIFACT_DIR" "$BENCH_ARTIFACT_DIR/info" "$BENCH_ARTIFACT_DIR/raw" "$BENCH_ARTIFACT_DIR/logs"

# ---------------------------------------------------------------------------
# Pre-flight: abort if any core is already saturated
# ---------------------------------------------------------------------------
cores=$(nproc)
load1=$(awk '{print $1}' /proc/loadavg)
threshold=$(awk "BEGIN {printf \"%.1f\", $cores * $BENCH_LOAD_FACTOR}")
if awk "BEGIN {exit !($load1 > $threshold)}"; then
  echo "ERROR: 1-min load average $load1 exceeds threshold $threshold (${cores} cores × ${BENCH_LOAD_FACTOR})." >&2
  echo "       Wait for background work to finish before recording benchmarks." >&2
  exit 3
fi
echo "load check passed: load=$load1 threshold=$threshold"

COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
DIRTY=false
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  if ! git diff --quiet || ! git diff --cached --quiet; then DIRTY=true; fi
fi

HOST="$(hostname)"
SYS_PINNED_CPUS="${BENCH_PIN_CPUS:-}"

# ---------------------------------------------------------------------------
# System metadata helpers (preset-independent fields)
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

artifact_slug() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9_.-' '_'
}

SYS_CPU=$(cpu_model)
SYS_CORES=$(cpu_cores)
SYS_SMT=$(cpu_smt)
SYS_KERNEL=$(kernel_ver)
SYS_LIBC=$(libc_ver)
SYS_GOVERNOR=$(governor)

make_metadata() {
  local mode="$1" preset="${2:-}"
  jq -nc \
    --arg cpu "$SYS_CPU" \
    --argjson cores "$SYS_CORES" \
    --arg smt "$SYS_SMT" \
    --arg kernel "$SYS_KERNEL" \
    --arg libc "$SYS_LIBC" \
    --arg governor "$SYS_GOVERNOR" \
    --arg compiler_version "$SYS_COMPILER_VER" \
    --arg pinned_cpus "$SYS_PINNED_CPUS" \
    --arg artifact_dir "$BENCH_ARTIFACT_DIR" \
    --arg mode "$mode" \
    --arg preset "$preset" \
    --argjson reps "$BENCH_REPS" \
    '{cpu:$cpu, cores:$cores, smt:$smt, kernel:$kernel, libc:$libc,
      governor:$governor, compiler_version:$compiler_version,
      pinned_cpus:$pinned_cpus, artifact_dir:$artifact_dir,
      mode:$mode, preset:$preset, reps:$reps}'
}

write_manifest() {
  local mode="record" preset_words="$BENCH_PRESETS" presets_json
  if $COMPARE_MODE; then
    mode="compare"
    preset_words="${COMPARE_PRESETS[*]}"
  elif $COMPARE_BINS_MODE; then
    mode="compare-bins"
    preset_words=""
  fi
  read -r -a manifest_presets <<< "$preset_words"
  if ((${#manifest_presets[@]} > 0)); then
    presets_json=$(printf '%s\n' "${manifest_presets[@]}" | jq -R . | jq -s .)
  else
    presets_json='[]'
  fi
  jq -nc \
    --arg name "$NAME" \
    --arg mode "$mode" \
    --arg repo_root "$REPO_ROOT" \
    --arg commit "$COMMIT" \
    --arg branch "$BRANCH" \
    --argjson dirty "$DIRTY" \
    --arg host "$HOST" \
    --arg machine_id "$MACHINE_ID" \
    --arg created_utc "$RUN_STAMP" \
    --arg artifact_dir "$BENCH_ARTIFACT_DIR" \
    --argjson presets "$presets_json" \
    --argjson reps "$BENCH_REPS" \
    '{name:$name, repo_root:$repo_root, commit_sha:$commit, branch:$branch,
      mode:$mode, dirty:$dirty, host:$host, machine_id:$machine_id,
      created_utc:$created_utc, artifact_dir:$artifact_dir,
      presets:$presets, reps:$reps}' \
    > "$BENCH_ARTIFACT_DIR/manifest.json"
  echo "artifact dir: $BENCH_ARTIFACT_DIR"
}

write_manifest

# ---------------------------------------------------------------------------
# Pre-bench cleanup: remove debug trees to free tmpfs RAM before any builds
# ---------------------------------------------------------------------------
PROJECT_TMP="/tmp/$(basename "$REPO_ROOT")"
if [[ -d "$PROJECT_TMP" ]]; then
  for tree in "$PROJECT_TMP"/debug-*; do
    [[ -d "$tree" ]] || continue
    echo "removing debug tree: $tree"
    rm -rf "$tree"
  done
fi

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

cache_value() {
  local build_dir="$1" key="$2"
  sed -n "s/^${key}:[A-Z_]*=//p" "$build_dir/CMakeCache.txt" | tail -1
}

copy_build_cache_artifact() {
  local preset="$1" build_dir="$2"
  mkdir -p "$BENCH_ARTIFACT_DIR/cache"
  cp "$build_dir/CMakeCache.txt" "$BENCH_ARTIFACT_DIR/cache/$(artifact_slug "$preset")-CMakeCache.txt"
}

assert_recording_cache() {
  local preset="$1" build_dir="$2"
  local asan ubsan tsan benches tests lto build_type
  asan="$(cache_value "$build_dir" CONFLUX_ENABLE_ASAN)"
  ubsan="$(cache_value "$build_dir" CONFLUX_ENABLE_UBSAN)"
  tsan="$(cache_value "$build_dir" CONFLUX_ENABLE_TSAN)"
  benches="$(cache_value "$build_dir" CONFLUX_BUILD_BENCHMARKS)"
  tests="$(cache_value "$build_dir" CONFLUX_BUILD_TESTS)"
  lto="$(cache_value "$build_dir" CONFLUX_ENABLE_LTO)"
  build_type="$(cache_value "$build_dir" CMAKE_BUILD_TYPE)"

  if [[ "$asan" != OFF || "$ubsan" != OFF || "$tsan" != OFF ]]; then
    if [[ "$CONFLUX_ALLOW_SANITIZED_BENCHMARKS" != ON ]]; then
      echo "$preset enables sanitizers; set CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON only for local benchmark debugging." >&2
      return 1
    fi
  fi

  if [[ "$benches" != ON ]]; then
    echo "$preset does not build benchmark targets." >&2
    return 1
  fi

  if [[ "$ALLOW_NON_PERF_BENCH_PRESET" == 1 ]]; then
    echo "warning: ALLOW_NON_PERF_BENCH_PRESET=1 accepts $preset as an explicit experiment." >&2
    return 0
  fi

  if [[ "$preset" != perf-* ]]; then
    echo "$preset is not a perf-* preset; set ALLOW_NON_PERF_BENCH_PRESET=1 for explicit experiments." >&2
    return 1
  fi
  if [[ "$tests" != OFF ]]; then
    echo "$preset builds test targets; perf recordings must stay benchmark-only." >&2
    return 1
  fi
  if [[ "$lto" != OFF ]]; then
    echo "$preset enables LTO; perf recordings must stay symbolized/profile-friendly." >&2
    return 1
  fi
  if [[ "$build_type" != RelWithDebInfo ]]; then
    echo "$preset uses CMAKE_BUILD_TYPE=$build_type; perf recordings require RelWithDebInfo." >&2
    return 1
  fi
}

result_row_count() {
  jq -Rsc '[split("\n")[] | select(length > 0) | try fromjson catch empty
           | select((.variant != null) and (.iterations != null)
                    and (.total_ns != null) and (.ns_per_iter != null))] | length' "$1"
}

require_result_rows() {
  local file="$1" bench="$2"
  local rows
  rows="$(result_row_count "$file")"
  if [[ "$rows" == 0 ]]; then
    echo "benchmark $bench produced no valid NDJSON result rows; raw artifact: $file" >&2
    return 1
  fi
}

declare -A ITERATIONS_FROM_DB=()
load_iteration_overrides() {
  local run_id="$1"
  if [[ -n "$run_id" ]]; then
    [[ "$run_id" =~ ^[0-9]+$ ]] || {
      echo "BENCH_ITERATIONS_FROM_RUN_ID must be numeric: $run_id" >&2
      exit 2
    }

    while IFS=$'\t' read -r bench cfg iters; do
      [[ -n "$bench" && -n "$cfg" && -n "$iters" ]] || continue
      ITERATIONS_FROM_DB["$bench|$cfg"]="$iters"
    done < <(
      psql "$PGURI" -At -q -c "
        SELECT r.benchmark, runs.config_name, r.iterations
        FROM results r
        JOIN runs ON runs.id = r.run_id
        WHERE r.run_id = $run_id AND r.extra->>'kind' = 'summary'
        ORDER BY r.benchmark, runs.config_name;"
    )
    return 0
  fi

  while IFS=$'\t' read -r bench cfg iters; do
    [[ -n "$bench" && -n "$cfg" && -n "$iters" ]] || continue
    ITERATIONS_FROM_DB["$bench|$cfg"]="$iters"
  done < <(
    psql "$PGURI" -At -q -c "
      SELECT DISTINCT ON (r.benchmark, runs.config_name)
             r.benchmark, runs.config_name, r.iterations
      FROM results r
      JOIN runs ON runs.id = r.run_id
      WHERE r.extra->>'kind' = 'summary'
      ORDER BY r.benchmark, runs.config_name, runs.created_at DESC, r.run_id DESC;"
  )
}

load_iteration_overrides "$BENCH_ITERATIONS_FROM_RUN_ID"

rewrite_args_for_iterations() {
  local bench="$1" cfg="$2"
  shift 2
  local -a args=("$@")
  local key="$bench|$cfg"
  local override="${ITERATIONS_FROM_DB[$key]-}"
  if [[ -z "$override" ]]; then
    printf '%s\n' "${args[@]}"
    return 0
  fi

  local warmup=$(( override / 5 ))
  (( warmup < 1 )) && warmup=1

  local -a out=()
  local saw_iters=false
  local saw_warmup=false
  for ((i=0; i<${#args[@]}; i++)); do
    case "${args[$i]}" in
      --iterations)
        out+=(--iterations "$override")
        saw_iters=true
        ((i++))
        ;;
      --warmup)
        out+=(--warmup "$warmup")
        saw_warmup=true
        ((i++))
        ;;
      *)
        out+=("${args[$i]}")
        ;;
    esac
  done

  if ! $saw_iters; then
    out+=(--iterations "$override")
  fi
  if ! $saw_warmup; then
    out+=(--warmup "$warmup")
  fi

  printf '%s\n' "${out[@]}"
}

COMPARE_BENCH_NAME=""
COMPARE_CFG_NAME=""
COMPARE_BIN_ARGS=()
COMPARE_BENCH_INFO=""
load_bench_info_args() {
  local binary="$1"
  local info cfg_json
  info=$("$binary" --bench-info 2>/dev/null) || {
    echo "binary does not support --bench-info: $binary" >&2
    exit 2
  }
  COMPARE_BENCH_INFO="$info"
  COMPARE_BENCH_NAME=$(jq -r '.name' <<< "$info")
  cfg_json=$(jq -c '.configs[0] // empty' <<< "$info")
  if [[ -z "$cfg_json" ]]; then
    COMPARE_CFG_NAME="default"
    COMPARE_BIN_ARGS=()
    return 0
  fi
  COMPARE_CFG_NAME=$(jq -r '.name // "default"' <<< "$cfg_json")
  mapfile -t COMPARE_BIN_ARGS < <(jq -r '.args[]? // empty' <<< "$cfg_json")
}

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

standard_extra_jq='del(.config, .variant, .iterations, .total_ns, .ns_per_iter) | if . == {} then {} else . end'
standard_rows_jq='try (fromjson
  | [.variant, .iterations, .total_ns, .ns_per_iter, (('$standard_extra_jq') | @json)]
  | @tsv)'

# record_with_reps: runs bench several times, inserts raw rows + summary row.
# Args: run_id bench_name reps <bench_args_to_produce_ndjson>...
# Expects NDJSON: {"config":"","variant":"","iterations":N,"total_ns":N,"ns_per_iter":X}
# Optional fields are preserved into raw results.extra; summary rows still use min/p10/mad when present.
record_with_reps() {
  local run_id="$1" bench="$2" reps="$3"; shift 3
  local tmpf rawf
  tmpf=$(mktemp /tmp/bench_reps_XXXXXX.ndjson)
  trap 'rm -f "$tmpf"' RETURN
  rawf="$BENCH_ARTIFACT_DIR/raw/run_${run_id}_$(artifact_slug "$bench").ndjson"

  local i; for i in $(seq 1 "$reps"); do
    "$@" 2>/dev/null >> "$tmpf"
  done
  require_result_rows "$tmpf" "$bench"
  cp "$tmpf" "$rawf"

  while IFS=$'\t' read -r variant iters total ns_pi ex; do
    psql "$PGURI" -At -q -c "
      INSERT INTO results
        (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
         metric, value, unit, sample_count, extra)
      VALUES
        ($run_id, '$(sql_escape "$bench")', '$(sql_escape "$variant")',
         $iters, $total, $ns_pi,
         'ns_per_iter', $ns_pi, 'ns', 1, '$(sql_escape "$ex")'::jsonb);" >/dev/null
  done < <(jq -r -R "$standard_rows_jq" "$tmpf")

  psql "$PGURI" -At -q -c "
    WITH raw AS (
      SELECT variant,
             PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS med,
             PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS p50,
             PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY ns_per_iter) AS p99,
             MIN(COALESCE((extra->>'min')::double precision, ns_per_iter)) AS best,
             PERCENTILE_CONT(0.5) WITHIN GROUP (
               ORDER BY COALESCE((extra->>'p10')::double precision, ns_per_iter)) AS p10,
             COUNT(*) AS n,
             AVG(iterations) AS avg_iters
      FROM results
      WHERE run_id = $run_id AND benchmark = '$(sql_escape "$bench")'
        AND COALESCE(extra->>'kind', '') <> 'summary'
      GROUP BY variant
    ),
    mad_raw AS (
      SELECT r.variant, raw.med,
             PERCENTILE_CONT(0.5) WITHIN GROUP (
               ORDER BY ABS(r.ns_per_iter - raw.med)) AS mad,
             raw.p50, raw.p99, raw.best, raw.p10, raw.avg_iters, raw.n
      FROM results r
      JOIN raw ON raw.variant = r.variant
      WHERE r.run_id = $run_id AND r.benchmark = '$(sql_escape "$bench")'
        AND COALESCE(r.extra->>'kind', '') <> 'summary'
      GROUP BY r.variant, raw.med, raw.p50, raw.p99, raw.best, raw.p10,
               raw.avg_iters, raw.n
    )
    INSERT INTO results
      (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
       metric, value, unit, sample_count, median, mad, p50, p99, best, p10, extra)
    SELECT $run_id, '$(sql_escape "$bench")', variant,
           avg_iters::bigint, 0, med,
           'ns_per_iter', med, 'ns', n::bigint,
           med, mad, p50, p99, best, p10,
           '{\"kind\":\"summary\"}'::jsonb
    FROM mad_raw;" >/dev/null
}

want() { [[ -z "${ONLY_BENCH:-}" || "${ONLY_BENCH}" == "$1" ]]; }

# ---------------------------------------------------------------------------
# --compare mode: interleaved multi-preset thermal-fair comparison
#
# Usage: bench_record.sh --compare preset1 preset2 [preset3 ...]
#   Builds each preset once, then runs BENCH_REPS rounds.
#   Each round rotates candidate order (offset = (round-1) % n).
#   Constant 2s settle before every bench execution.
#   Load check before every execution; abort if overloaded.
#   Results tagged with {"round":N,"position":P,...} in extra JSONB.
# ---------------------------------------------------------------------------
load_check_or_abort() {
  local cores thr load
  cores=$(nproc)
  load=$(awk '{print $1}' /proc/loadavg)
  thr=$(awk "BEGIN {printf \"%.1f\", $cores * $BENCH_LOAD_FACTOR}")
  if awk "BEGIN {exit !($load > $thr)}"; then
    echo "ERROR: load $load > threshold $thr before bench run — aborting compare" >&2
    exit 3
  fi
}

run_compare() {
  local presets=("$@")
  local n=${#presets[@]}
  [[ $n -ge 2 ]] || { echo "compare requires at least 2 presets" >&2; exit 1; }

  # Build each preset and create its run_id (no cleanup between builds).
  declare -A build_dirs run_ids
  for p in "${presets[@]}"; do
    echo "=== building $p ==="
    PRESET="$p"
    COMPILER="clang++"
    case "$p" in *gcc*) COMPILER="g++" ;; esac
    SYS_COMPILER_VER=$(compiler_version)
    METADATA=$(make_metadata compare "$p")
    local cfg_log bdir
    cfg_log=$(cmake --preset "$p" 2>&1)
    printf '%s\n' "$cfg_log" > "$BENCH_ARTIFACT_DIR/logs/$(artifact_slug "$p")-configure.log"
    bdir=$(printf '%s\n' "$cfg_log" | sed -n 's/^-- Build files have been written to: //p' | tail -1)
    if [[ -z "$bdir" || ! -d "$bdir" ]]; then
      echo "configure failed for preset $p" >&2
      printf '%s\n' "$cfg_log" | tail -20 >&2
      exit 2
    fi
    assert_recording_cache "$p" "$bdir"
    copy_build_cache_artifact "$p" "$bdir"
    local build_log="$BENCH_ARTIFACT_DIR/logs/$(artifact_slug "$p")-build.log"
    if ! cmake --build "$bdir" --target conflux_work_benchmarks \
        > "$build_log" 2>&1; then
      echo "build failed for preset $p; log=$build_log" >&2
      tail -40 "$build_log" >&2
      exit 2
    fi
    build_dirs[$p]="$bdir"
    run_ids[$p]=$(new_run "work" "compare" "{\"compare\":true,\"preset\":\"$p\"}")
    echo "  run_id=${run_ids[$p]}"
  done

  echo "all presets built — settling ${BENCH_SETTLE_AFTER_BUILD_SEC}s before compare rounds..."
  sleep "$BENCH_SETTLE_AFTER_BUILD_SEC"

  local reps="${BENCH_REPS:-5}"
  for round in $(seq 1 "$reps"); do
    echo "--- round $round/$reps ---"
    local offset=$(( (round - 1) % n ))
    local pos=0
    for ((j=0; j<n; j++)); do
      local idx=$(( (offset + j) % n ))
      local p="${presets[$idx]}"
      pos=$((j + 1))

      load_check_or_abort
      echo "  settle ${BENCH_SETTLE_BETWEEN_SEC}s before $p (round=$round pos=$pos)..."
      sleep "$BENCH_SETTLE_BETWEEN_SEC"

      local binary="${build_dirs[$p]}/benchmarks/conflux_work_benchmarks"
      local rid="${run_ids[$p]}"
      echo "  running $p (round=$round pos=$pos)..."

      local tmpf rawf
      tmpf=$(mktemp /tmp/compare_work_XXXXXX.ndjson)
      rawf="$BENCH_ARTIFACT_DIR/raw/run_${rid}_work_$(artifact_slug "$p")_r${round}.ndjson"
      run_bench "$binary" --json 2>/dev/null > "$tmpf"
      require_result_rows "$tmpf" work
      cp "$tmpf" "$rawf"
      while IFS=$'\t' read -r variant iters total ns_pi raw_ex; do
        local ex
        ex=$(jq -c --argjson raw "$raw_ex" --argjson round "$round" --argjson pos "$pos" \
          '$raw + {round:$round, position:$pos}' <<< '{}')
        psql "$PGURI" -At -q -c "
          INSERT INTO results
            (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
             metric, value, unit, sample_count, extra)
          VALUES
            ($rid, 'work', '$(sql_escape "$variant")',
             $iters, $total, $ns_pi,
             'ns_per_iter', $ns_pi, 'ns', 1, '$(sql_escape "$ex")'::jsonb);" >/dev/null
      done < <(jq -r -R "$standard_rows_jq" "$tmpf")
      rm -f "$tmpf"
    done
  done

  # Insert summary rows for each preset.
  for p in "${presets[@]}"; do
    local rid="${run_ids[$p]}"
    psql "$PGURI" -At -q -c "
      WITH raw AS (
        SELECT variant,
               PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS med,
               PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS p50,
               PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY ns_per_iter) AS p99,
               MIN((extra->>'min')::double precision) AS best,
               PERCENTILE_CONT(0.5) WITHIN GROUP (
                 ORDER BY (extra->>'p10')::double precision) AS p10,
               COUNT(*) AS n,
               AVG(iterations) AS avg_iters
        FROM results
        WHERE run_id = $rid AND (extra->>'min') IS NOT NULL
          AND (extra->>'round') IS NOT NULL
        GROUP BY variant
      ),
      mad_raw AS (
        SELECT r.variant, raw.med,
               PERCENTILE_CONT(0.5) WITHIN GROUP (
                 ORDER BY ABS(r.ns_per_iter - raw.med)) AS mad,
               raw.p50, raw.p99, raw.best, raw.p10, raw.avg_iters, raw.n
        FROM results r
        JOIN raw ON raw.variant = r.variant
        WHERE r.run_id = $rid AND (r.extra->>'min') IS NOT NULL
          AND (r.extra->>'round') IS NOT NULL
        GROUP BY r.variant, raw.med, raw.p50, raw.p99, raw.best, raw.p10,
                 raw.avg_iters, raw.n
      )
      INSERT INTO results
        (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
         metric, value, unit, sample_count, median, mad, p50, p99, best, p10, extra)
      SELECT $rid, 'work', variant,
             avg_iters::bigint, 0, med,
             'ns_per_iter', med, 'ns', n::bigint,
             med, mad, p50, p99, best, p10,
             '{\"kind\":\"summary\"}'::jsonb
      FROM mad_raw;" >/dev/null
    echo "summary inserted for $p (run_id=$rid)"
  done

  echo "compare done."
}

# ---------------------------------------------------------------------------
# --compare-bins mode: interleaved comparison with pre-built binaries
#
# Usage: bench_record.sh --compare-bins label1:path1 label2:path2 [...]
#   Each argument is "label:binary_path". Binaries must already be built.
#   Runs BENCH_REPS rounds with rotating candidate order.
#   Same 2s settle + load check before every execution as --compare.
# ---------------------------------------------------------------------------
_compare_bins_insert_row() {
  local rid="$1" bench="$2" label="$3" round="$4" pos="$5"; shift 5
  local tmpf rawf
  tmpf=$(mktemp /tmp/compare_bins_XXXXXX.ndjson)
  rawf="$BENCH_ARTIFACT_DIR/raw/run_${rid}_$(artifact_slug "$bench")_$(artifact_slug "$label")_r${round}.ndjson"

  run_bench "$@" --json 2>/dev/null >> "$tmpf"
  require_result_rows "$tmpf" "$bench"
  cp "$tmpf" "$rawf"

  while IFS=$'\t' read -r variant iters total ns_pi raw_ex; do
    local ex
    ex=$(jq -c --argjson raw "$raw_ex" --arg label "$label" --argjson round "$round" --argjson pos "$pos" \
      '$raw + {round:$round, position:$pos, label:$label}' <<< '{}')
    insert_row "$rid" "$bench" "$variant" "$iters" "$total" "$ns_pi" "$ex"
  done < <(jq -r -R "$standard_rows_jq" "$tmpf")

  rm -f "$tmpf"
}

_compare_bins_insert_summary() {
  local rid="$1" bench="$2"
  psql "$PGURI" -At -q -c "
    WITH raw AS (
      SELECT variant,
             PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS med,
             PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS p50,
             PERCENTILE_CONT(0.99) WITHIN GROUP (ORDER BY ns_per_iter) AS p99,
             MIN((extra->>'min')::double precision) AS best,
             PERCENTILE_CONT(0.5) WITHIN GROUP (
               ORDER BY (extra->>'p10')::double precision) AS p10,
             COUNT(*) AS n,
             AVG(iterations) AS avg_iters
      FROM results
      WHERE run_id = $rid AND (extra->>'round') IS NOT NULL
      GROUP BY variant
    ),
    mad_raw AS (
      SELECT r.variant, raw.med,
             PERCENTILE_CONT(0.5) WITHIN GROUP (
               ORDER BY ABS(r.ns_per_iter - raw.med)) AS mad,
             raw.p50, raw.p99, raw.best, raw.p10, raw.avg_iters, raw.n
      FROM results r
      JOIN raw ON raw.variant = r.variant
      WHERE r.run_id = $rid AND (r.extra->>'round') IS NOT NULL
      GROUP BY r.variant, raw.med, raw.p50, raw.p99, raw.best, raw.p10,
               raw.avg_iters, raw.n
    )
    INSERT INTO results
      (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
       metric, value, unit, sample_count, median, mad, p50, p99, best, p10, extra)
    SELECT $rid, '$(sql_escape "$bench")', variant,
           avg_iters::bigint, 0, med,
           'ns_per_iter', med, 'ns', n::bigint,
           med, mad, p50, p99, best, p10,
           '{\"kind\":\"summary\"}'::jsonb
    FROM mad_raw;" >/dev/null
}

run_compare_bins() {
  # args: label1:binary1 label2:binary2 ...
  local n=$#
  [[ $n -ge 2 ]] || { echo "compare-bins requires at least 2 candidates" >&2; exit 1; }

  local labels=() binaries=() bench_name="" cfg_name=""
  for arg in "$@"; do
    local label="${arg%%:*}" bin="${arg#*:}"
    [[ -x "$bin" ]] || { echo "binary not found or not executable: $bin" >&2; exit 1; }
    load_bench_info_args "$bin"
    printf '%s\n' "$COMPARE_BENCH_INFO" > "$BENCH_ARTIFACT_DIR/info/compare-bins_$(artifact_slug "$label").json"
    if [[ -z "$bench_name" ]]; then
      bench_name="$COMPARE_BENCH_NAME"
      cfg_name="$COMPARE_CFG_NAME"
    elif [[ "$bench_name" != "$COMPARE_BENCH_NAME" || "$cfg_name" != "$COMPARE_CFG_NAME" ]]; then
      echo "compare-bins candidates must agree on bench/config: got ${COMPARE_BENCH_NAME} [$COMPARE_CFG_NAME] from $bin" >&2
      echo "expected ${bench_name} [$cfg_name]" >&2
      exit 2
    fi
    labels+=("$label")
    binaries+=("$bin")
  done

  echo "candidates:"
  for ((i=0; i<n; i++)); do
    echo "  [${labels[$i]}] ${binaries[$i]}"
  done

  # Create run_ids: use git commit of the REPO_ROOT (where script is invoked).
  COMPILER="clang++"  # best-effort; refine via label naming if needed
  SYS_COMPILER_VER=$(compiler_version)
  METADATA=$(make_metadata compare-bins)

  local run_ids=()
  for ((i=0; i<n; i++)); do
    PRESET="${labels[$i]}"
    local rid
    rid=$(new_run "$bench_name" "compare-bins" "{\"compare\":true,\"label\":\"${labels[$i]}\"}")
    run_ids+=("$rid")
    echo "  ${labels[$i]} → run_id=$rid"
  done

  local reps="${BENCH_REPS:-5}"
  for round in $(seq 1 "$reps"); do
    echo "--- round $round/$reps ---"
    local offset=$(( (round - 1) % n ))
    for ((j=0; j<n; j++)); do
      local idx=$(( (offset + j) % n ))
      local pos=$((j + 1))
      local label="${labels[$idx]}"
      local bin="${binaries[$idx]}"
      local rid="${run_ids[$idx]}"

      load_check_or_abort
      echo "  settle ${BENCH_SETTLE_BETWEEN_SEC}s before ${label} (round=$round pos=$pos)..."
      sleep "$BENCH_SETTLE_BETWEEN_SEC"
      echo "  running ${label}..."
      load_bench_info_args "$bin"
      local -a args=("${COMPARE_BIN_ARGS[@]}")
      if [[ -n "$BENCH_ITERATIONS_FROM_RUN_ID" ]]; then
        mapfile -t args < <(rewrite_args_for_iterations "$COMPARE_BENCH_NAME" "$COMPARE_CFG_NAME" "${args[@]}")
      fi
      _compare_bins_insert_row "$rid" "$bench_name" "$label" "$round" "$pos" "$bin" "${args[@]}"
    done
  done

  for ((i=0; i<n; i++)); do
    _compare_bins_insert_summary "${run_ids[$i]}" "$bench_name"
    echo "summary inserted for ${labels[$i]} (run_id=${run_ids[$i]})"
  done

  echo "compare-bins done."
}

# ---------------------------------------------------------------------------
# Custom parsers (for benches with non-standard output fields)
# ---------------------------------------------------------------------------

# file_copy_coro — NDJSON with extra mib/best fields; configs from --bench-info JSON
run_file_copy() {
  local binary="$1" bench="$2" cfg_name="$3" extra="$4"
  shift 4
  local args=("$@")
  local RID tmpf rawf
  RID=$(new_run "$bench" "$cfg_name" "$extra")
  echo "+ $bench [$cfg_name] run_id=$RID"
  tmpf=$(mktemp /tmp/file_copy_XXXXXX.ndjson)
  rawf="$BENCH_ARTIFACT_DIR/raw/run_${RID}_$(artifact_slug "$bench")_$(artifact_slug "$cfg_name").ndjson"
  run_bench "$binary" "${args[@]}" --json 2>/dev/null > "$tmpf"
  require_result_rows "$tmpf" "$bench"
  cp "$tmpf" "$rawf"
  jq -r -R 'try (fromjson | [.variant, .iterations, .total_ns, .ns_per_iter, (.avg_mib_per_s // 0), (.best_mib_per_s // 0), (.best_ns // 0)] | @tsv)' "$tmpf" \
    | while IFS=$'\t' read -r variant iters total_ns ns_pi avg_mibs best_mibs best_ns; do
        local ex
        ex=$(printf '{"avg_mib_per_s":%s,"best_mib_per_s":%s,"best_ns":%s}' "$avg_mibs" "$best_mibs" "$best_ns")
        insert_row "$RID" "$bench" "$variant" "$iters" "$total_ns" "$ns_pi" "$ex"
      done
  rm -f "$tmpf"
}

# ---------------------------------------------------------------------------
# Per-preset build → run → delete loop
# ---------------------------------------------------------------------------
clean_build() {
  local dir="$1"
  if [[ "${KEEP_BUILD:-0}" != "1" && "$dir" == /tmp/* && -d "$dir" ]]; then
    rm -rf "$dir"
    echo "cleaned $dir"
  fi
}

CURRENT_BUILD_DIR=""
trap '[[ -n "$CURRENT_BUILD_DIR" ]] && clean_build "$CURRENT_BUILD_DIR"' EXIT

if $COMPARE_MODE; then
  run_compare "${COMPARE_PRESETS[@]}"
  exit 0
fi

if $COMPARE_BINS_MODE; then
  run_compare_bins "${COMPARE_BINS_ARGS[@]}"
  exit 0
fi

read -r -a PRESET_LIST <<< "$BENCH_PRESETS"
preset_count=${#PRESET_LIST[@]}
preset_idx=0

for PRESET in "${PRESET_LIST[@]}"; do
  preset_idx=$((preset_idx + 1))
  echo "=== preset $preset_idx/$preset_count: $PRESET ==="

  COMPILER="clang++"
  case "$PRESET" in *gcc*) COMPILER="g++" ;; esac
  SYS_COMPILER_VER=$(compiler_version)

  METADATA=$(make_metadata record "$PRESET")

  # Build
  CONFIGURE_LOG=$(cmake --preset "$PRESET" 2>&1)
  printf '%s\n' "$CONFIGURE_LOG" > "$BENCH_ARTIFACT_DIR/logs/$(artifact_slug "$PRESET")-configure.log"
  BUILD_DIR=$(printf '%s\n' "$CONFIGURE_LOG" | sed -n 's/^-- Build files have been written to: //p' | tail -1)
  if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
    echo "configure failed; preset=$PRESET" >&2
    printf '%s\n' "$CONFIGURE_LOG" | tail -20 >&2
    exit 2
  fi

  assert_recording_cache "$PRESET" "$BUILD_DIR"
  copy_build_cache_artifact "$PRESET" "$BUILD_DIR"

  CURRENT_BUILD_DIR="$BUILD_DIR"
  BUILD_LOG="$BENCH_ARTIFACT_DIR/logs/$(artifact_slug "$PRESET")-build.log"
  if ! cmake --build "$BUILD_DIR" --target conflux_record_benches \
      > "$BUILD_LOG" 2>&1; then
    echo "build failed; preset=$PRESET log=$BUILD_LOG" >&2
    tail -40 "$BUILD_LOG" >&2
    exit 2
  fi

  # Let CPU caches and frequency scaling settle after a full-core build.
  echo "build done — settling ${BENCH_SETTLE_AFTER_BUILD_SEC}s before benchmarks..."
  sleep "$BENCH_SETTLE_AFTER_BUILD_SEC"

  BENCHDIR="$BUILD_DIR/benchmarks"
  # Auto-discovery loop for this preset
  for binary in "$BENCHDIR"/conflux_*bench*; do
    [[ -x "$binary" ]] || continue

    info=$("$binary" --bench-info 2>/dev/null) || {
      echo "skip $(basename "$binary"): no --bench-info support" >&2
      continue
    }
    printf '%s\n' "$info" > "$BENCH_ARTIFACT_DIR/info/${PRESET}_$(basename "$binary").json"

    bench_name=$(jq -r .name <<< "$info" 2>/dev/null || true)
    if [[ -z "$bench_name" || "$bench_name" == "null" ]]; then
      echo "skip $(basename "$binary"): --bench-info returned invalid JSON" >&2
      continue
    fi
    parser=$(jq -r .parser <<< "$info")

    want "$bench_name" || continue

    case "$parser" in
      standard|file_copy) ;;
      *)
        echo "unsupported parser from $(basename "$binary"): '$parser'" >&2
        exit 2
        ;;
    esac

    # Iterate configs from --bench-info JSON
    while IFS= read -r cfg_json; do
      cfg_name=$(jq -r .name <<< "$cfg_json")
      extra=$(jq -c '.extra // {}' <<< "$cfg_json")
      cfg_reps=$(jq -r '.reps // empty' <<< "$cfg_json")
      [[ -n "$cfg_reps" ]] || cfg_reps="$BENCH_REPS"
      if ! [[ "$cfg_reps" =~ ^[1-9][0-9]*$ ]]; then
        echo "invalid reps for $bench_name [$cfg_name]: $cfg_reps" >&2
        exit 2
      fi
      mapfile -t args < <(jq -r '.args[]?' <<< "$cfg_json")
      if [[ -n "$BENCH_ITERATIONS_FROM_RUN_ID" ]]; then
        mapfile -t args < <(rewrite_args_for_iterations "$bench_name" "$cfg_name" "${args[@]}")
      fi

      if [[ "$parser" == "file_copy" ]]; then
        run_file_copy "$binary" "$bench_name" "$cfg_name" "$extra" "${args[@]}"
        sleep 1
        continue
      fi

      RID=$(new_run "$bench_name" "$cfg_name" "$extra")
      echo "+ $bench_name [$cfg_name] run_id=$RID"

      record_with_reps "$RID" "$bench_name" "$cfg_reps" \
        run_bench "$binary" "${args[@]}" --json
      sleep 1
    done < <(jq -c '.configs[]' <<< "$info")
  done

  echo "done: $PRESET"

  # Delete this build tree immediately before starting the next preset.
  clean_build "$BUILD_DIR"
  CURRENT_BUILD_DIR=""

  if (( preset_idx < preset_count )); then
    echo "settling ${BENCH_SETTLE_BETWEEN_SEC}s before next preset..."
    sleep "$BENCH_SETTLE_BETWEEN_SEC"
  fi
done

echo "all done."
