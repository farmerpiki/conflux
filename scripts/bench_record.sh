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
#
# --bench-info JSON:
#   {
#     "name":    "logical_bench_name",          # used as runs.benchmark
#     "parser":  "standard|tcp_parallel|file_copy",
#     "configs": [
#       { "name": "cfg", "extra": {}, "args": ["--iterations","N",...] }
#     ]
#   }
#   Parsers:
#     standard    — NDJSON variant,iterations,total_ns,ns_per_iter; uses record_with_reps
#     tcp_parallel — custom parser; configs read from $CONFIGS_DIR/*.json (args/extra ignored)
#     file_copy   — custom parser; configs come from --bench-info JSON
#
# Usage:
#   PGURI=postgres://postgres@localhost/conflux_bench scripts/bench_record.sh [run-name]
#
# Env knobs:
#   PGURI              postgres URI (default: postgres://postgres@localhost/conflux_bench)
#   BENCH_PRESET       space-separated cmake preset(s) (default: release-clang-libcxx release-gcc-stdcxx)
#                      each preset is built, run, and deleted before the next one starts
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
export PG_CONNINFO="${PG_CONNINFO:-host=localhost dbname=conflux_bench user=postgres}"
BENCH_PRESETS="${BENCH_PRESET:-release-clang-libcxx release-gcc-stdcxx}"
BENCH_REPS="${BENCH_REPS:-5}"
MACHINE_ID="${MACHINE_ID:-$(cat /etc/machine-id 2>/dev/null || hostname)}"
WAIVER_REASON="${WAIVER_REASON:-}"

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

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# ---------------------------------------------------------------------------
# Pre-flight: abort if any core is already saturated
# ---------------------------------------------------------------------------
cores=$(nproc)
load1=$(awk '{print $1}' /proc/loadavg)
threshold=$(awk "BEGIN {printf \"%.1f\", $cores * 1.5}")
if awk "BEGIN {exit !($load1 > $threshold)}"; then
  echo "ERROR: 1-min load average $load1 exceeds threshold $threshold (${cores} cores × 1.5)." >&2
  echo "       Wait for background work to finish before recording benchmarks." >&2
  exit 3
fi
echo "load check passed: load=$load1 threshold=$threshold"

COMMIT="$(git rev-parse HEAD)"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
DIRTY=false
if ! git diff --quiet || ! git diff --cached --quiet; then DIRTY=true; fi

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

SYS_CPU=$(cpu_model)
SYS_CORES=$(cpu_cores)
SYS_SMT=$(cpu_smt)
SYS_KERNEL=$(kernel_ver)
SYS_LIBC=$(libc_ver)
SYS_GOVERNOR=$(governor)

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
# Args: run_id bench_name <bench_args_to_produce_ndjson>...
# Expects NDJSON: {"config":"","variant":"","iterations":N,"total_ns":N,"ns_per_iter":X,"min":X,"p10":X,"mad":X}
record_with_reps() {
  local run_id="$1" bench="$2"; shift 2
  local reps="${BENCH_REPS:-5}"
  local tmpf
  tmpf=$(mktemp /tmp/bench_reps_XXXXXX.ndjson)
  trap 'rm -f "$tmpf"' RETURN

  local i; for i in $(seq 1 "$reps"); do
    "$@" 2>/dev/null >> "$tmpf"
  done

  while IFS=$'\t' read -r variant iters total ns_pi min_v p10_v mad_v; do
    local ex
    ex=$(printf '{"min":%s,"p10":%s,"mad":%s}' \
      "${min_v:-null}" "${p10_v:-null}" "${mad_v:-null}")
    psql "$PGURI" -At -q -c "
      INSERT INTO results
        (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
         metric, value, unit, sample_count, extra)
      VALUES
        ($run_id, '$(sql_escape "$bench")', '$(sql_escape "$variant")',
         $iters, $total, $ns_pi,
         'ns_per_iter', $ns_pi, 'ns', 1, '$(sql_escape "$ex")'::jsonb);" >/dev/null
  done < <(jq -r -R 'try (fromjson | [.variant, .iterations, .total_ns, .ns_per_iter, (.min // "null"), (.p10 // "null"), (.mad // "null")] | @tsv)' "$tmpf")

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
      WHERE run_id = $run_id AND benchmark = '$(sql_escape "$bench")'
        AND (extra->>'min') IS NOT NULL
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
        AND (r.extra->>'min') IS NOT NULL
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
  thr=$(awk "BEGIN {printf \"%.1f\", $cores * 1.5}")
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
}' "$SYS_CPU" "$SYS_CORES" "$SYS_SMT" "$SYS_KERNEL" \
       "$SYS_LIBC" "$SYS_GOVERNOR" "$SYS_COMPILER_VER" \
       "$SYS_PINNED_CPUS" "$BENCH_REPS")
    local cfg_log bdir
    cfg_log=$(cmake --preset "$p" 2>&1)
    bdir=$(printf '%s\n' "$cfg_log" | sed -n 's/^-- Build files have been written to: //p' | tail -1)
    if [[ -z "$bdir" || ! -d "$bdir" ]]; then
      echo "configure failed for preset $p" >&2
      printf '%s\n' "$cfg_log" | tail -20 >&2
      exit 2
    fi
    cmake --build "$bdir" --target conflux_work_benchmarks -- -j"$(nproc)" >/dev/null
    build_dirs[$p]="$bdir"
    run_ids[$p]=$(new_run "work" "compare" "{\"compare\":true,\"preset\":\"$p\"}")
    echo "  run_id=${run_ids[$p]}"
  done

  echo "all presets built — settling 20s before compare rounds..."
  sleep 20

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
      echo "  settle 2s before $p (round=$round pos=$pos)..."
      sleep 2

      local binary="${build_dirs[$p]}/benchmarks/conflux_work_benchmarks"
      local rid="${run_ids[$p]}"
      echo "  running $p (round=$round pos=$pos)..."

      while IFS=$'\t' read -r variant iters total ns_pi min_v p10_v mad_v; do
        local ex
        ex=$(printf '{"round":%d,"position":%d,"min":%s,"p10":%s,"mad":%s}' \
          "$round" "$pos" "${min_v:-null}" "${p10_v:-null}" "${mad_v:-null}")
        psql "$PGURI" -At -q -c "
          INSERT INTO results
            (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
             metric, value, unit, sample_count, extra)
          VALUES
            ($rid, 'work', '$(sql_escape "$variant")',
             $iters, $total, $ns_pi,
             'ns_per_iter', $ns_pi, 'ns', 1, '$(sql_escape "$ex")'::jsonb);" >/dev/null
      done < <(run_bench "$binary" --json 2>/dev/null \
        | jq -r -R 'try (fromjson | [.variant, .iterations, .total_ns, .ns_per_iter, (.min // "null"), (.p10 // "null"), (.mad // "null")] | @tsv)')
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
  local rid="$1" label="$2" round="$3" pos="$4"; shift 4
  while IFS=$'\t' read -r variant iters total ns_pi min_v p10_v mad_v; do
    local ex
    ex=$(printf '{"round":%d,"position":%d,"label":"%s","min":%s,"p10":%s,"mad":%s}' \
      "$round" "$pos" "$label" "${min_v:-null}" "${p10_v:-null}" "${mad_v:-null}")
    psql "$PGURI" -At -q -c "
      INSERT INTO results
        (run_id, benchmark, variant, iterations, total_ns, ns_per_iter,
         metric, value, unit, sample_count, extra)
      VALUES
        ($rid, 'work', '$(sql_escape "$variant")',
         $iters, $total, $ns_pi,
         'ns_per_iter', $ns_pi, 'ns', 1, '$(sql_escape "$ex")'::jsonb);" >/dev/null
  done < <(run_bench "$@" --json 2>/dev/null \
    | jq -r -R 'try (fromjson | [.variant, .iterations, .total_ns, .ns_per_iter, (.min // "null"), (.p10 // "null"), (.mad // "null")] | @tsv)')
}

_compare_bins_insert_summary() {
  local rid="$1"
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
    SELECT $rid, 'work', variant,
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

  local labels=() binaries=()
  for arg in "$@"; do
    local label="${arg%%:*}" bin="${arg#*:}"
    [[ -x "$bin" ]] || { echo "binary not found or not executable: $bin" >&2; exit 1; }
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
}' "$SYS_CPU" "$SYS_CORES" "$SYS_SMT" "$SYS_KERNEL" \
     "$SYS_LIBC" "$SYS_GOVERNOR" "$SYS_COMPILER_VER" \
     "$SYS_PINNED_CPUS" "$BENCH_REPS")

  local run_ids=()
  for ((i=0; i<n; i++)); do
    PRESET="${labels[$i]}"
    local rid
    rid=$(new_run "work" "compare-bins" "{\"compare\":true,\"label\":\"${labels[$i]}\"}")
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
      echo "  settle 2s before ${label} (round=$round pos=$pos)..."
      sleep 2
      echo "  running ${label}..."
      _compare_bins_insert_row "$rid" "$label" "$round" "$pos" "$bin"
    done
  done

  for ((i=0; i<n; i++)); do
    _compare_bins_insert_summary "${run_ids[$i]}"
    echo "summary inserted for ${labels[$i]} (run_id=${run_ids[$i]})"
  done

  echo "compare-bins done."
}

# ---------------------------------------------------------------------------
# Custom parsers (for benches with non-standard output fields)
# ---------------------------------------------------------------------------

# tcp_parallel_coro — per-parallelism NDJSON with extra fields; configs from CONFIGS_DIR/*.json
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
        --iterations 200 --warmup 50 --parallel 1,2,4,8 --config "$path" --json 2>/dev/null \
      | jq -r -R 'try (fromjson | [.variant, .iterations, .total_ns, .ns_per_iter, (.flags // ""), (.ring_entries // 0), (.throughput_iter_per_s // 0)] | @tsv)' \
      | while IFS=$'\t' read -r variant iters total_ns ns_pi flags ring tput; do
          local ex
          ex=$(printf '{"flags":"%s","ring_entries":%s,"throughput_iter_per_s":%s}' "$flags" "$ring" "$tput")
          insert_row "$RID" "$bench" "$variant" "$iters" "$total_ns" "$ns_pi" "$ex"
        done
  done
}

# file_copy_coro — NDJSON with extra mib/best fields; configs from --bench-info JSON
run_file_copy() {
  local binary="$1" bench="$2" cfg_name="$3" extra="$4"
  shift 4
  local args=("$@")
  local RID
  RID=$(new_run "$bench" "$cfg_name" "$extra")
  echo "+ $bench [$cfg_name] run_id=$RID"
  run_bench "$binary" "${args[@]}" --json 2>/dev/null \
    | jq -r -R 'try (fromjson | [.variant, .iterations, .total_ns, .ns_per_iter, (.avg_mib_per_s // 0), (.best_mib_per_s // 0), (.best_ns // 0)] | @tsv)' \
    | while IFS=$'\t' read -r variant iters total_ns ns_pi avg_mibs best_mibs best_ns; do
        local ex
        ex=$(printf '{"avg_mib_per_s":%s,"best_mib_per_s":%s,"best_ns":%s}' "$avg_mibs" "$best_mibs" "$best_ns")
        insert_row "$RID" "$bench" "$variant" "$iters" "$total_ns" "$ns_pi" "$ex"
      done
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

  # Build
  CONFIGURE_LOG=$(cmake --preset "$PRESET" 2>&1)
  BUILD_DIR=$(printf '%s\n' "$CONFIGURE_LOG" | sed -n 's/^-- Build files have been written to: //p' | tail -1)
  if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
    echo "configure failed; preset=$PRESET" >&2
    printf '%s\n' "$CONFIGURE_LOG" | tail -20 >&2
    exit 2
  fi

  CURRENT_BUILD_DIR="$BUILD_DIR"
  cmake --build "$BUILD_DIR" --target conflux_record_benches -- -j"$(nproc)" >/dev/null

  # Let CPU caches and frequency scaling settle after a full-core build.
  echo "build done — settling 20s before benchmarks..."
  sleep 20

  BENCHDIR="$BUILD_DIR/benchmarks"
  CONFIGS_DIR="${BENCH_CONFIGS_DIR:-$REPO_ROOT/benchmarks/configs}"
  [[ -d "$CONFIGS_DIR" ]] || CONFIGS_DIR="$REPO_ROOT/configs"

  # Auto-discovery loop for this preset
  for binary in "$BENCHDIR"/conflux_*bench*; do
    [[ -x "$binary" ]] || continue

    info=$("$binary" --bench-info 2>/dev/null) || {
      echo "skip $(basename "$binary"): no --bench-info support" >&2
      continue
    }

    bench_name=$(jq -r .name <<< "$info" 2>/dev/null || true)
    if [[ -z "$bench_name" || "$bench_name" == "null" ]]; then
      echo "skip $(basename "$binary"): --bench-info returned invalid JSON" >&2
      continue
    fi
    parser=$(jq -r .parser <<< "$info")

    want "$bench_name" || continue

    if [[ "$parser" == "tcp_parallel" ]]; then
      run_tcp_parallel "$binary" "$bench_name"
      sleep 1
      continue
    fi

    # Iterate configs from --bench-info JSON
    while IFS= read -r cfg_json; do
      cfg_name=$(jq -r .name <<< "$cfg_json")
      extra=$(jq -c .extra <<< "$cfg_json")
      mapfile -t args < <(jq -r '.args[]' <<< "$cfg_json")

      if [[ "$parser" == "file_copy" ]]; then
        run_file_copy "$binary" "$bench_name" "$cfg_name" "$extra" "${args[@]}"
        sleep 1
        continue
      fi

      RID=$(new_run "$bench_name" "$cfg_name" "$extra")
      echo "+ $bench_name [$cfg_name] run_id=$RID"

      record_with_reps "$RID" "$bench_name" \
        run_bench "$binary" "${args[@]}" --json
      sleep 1
    done < <(jq -c '.configs[]' <<< "$info")
  done

  echo "done: $PRESET"

  # Delete this build tree immediately before starting the next preset.
  clean_build "$BUILD_DIR"
  CURRENT_BUILD_DIR=""

  if (( preset_idx < preset_count )); then
    echo "settling 5s before next preset..."
    sleep 5
  fi
done

echo "all done."
