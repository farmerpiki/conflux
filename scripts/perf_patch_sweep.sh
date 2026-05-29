#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/perf_patch_sweep.sh [--patch PATH ...] [--patch-dir DIR ...] [--suite auto|json|http-client|http-server|http2|websocket|all]

Environment:
  PERF_PATCH_PRESETS    default: release-clang-libcxx release-gcc16-stdcxx
  PERF_PATCH_REPS       default: 5
  PERF_PATCH_PGURI      default: postgresql:///conflux_bench?user=postgres
  PERF_PATCH_REPORT     default: findings/perf_patch_sweeps/<timestamp>/report.md
  PERF_PATCH_ARTIFACTS  default: findings/perf_patch_sweeps/<timestamp>/artifacts
  PERF_PATCH_TARGETS    optional explicit CMake targets
  PERF_PATCH_BENCHES    optional explicit benchmark names
EOF
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

patches=()
patch_dirs=()
suite="auto"

while (($#)); do
  case "$1" in
    --patch)
      shift
      [[ $# -gt 0 ]] || { echo "--patch needs a path" >&2; exit 2; }
      patches+=("$1")
      ;;
    --patch-dir)
      shift
      [[ $# -gt 0 ]] || { echo "--patch-dir needs a path" >&2; exit 2; }
      patch_dirs+=("$1")
      ;;
    --suite)
      shift
      [[ $# -gt 0 ]] || { echo "--suite needs a value" >&2; exit 2; }
      suite="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unexpected argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

case "$suite" in
  http) suite="http-server" ;;
  h2) suite="http2" ;;
  ws) suite="websocket" ;;
esac

case "$suite" in
  auto|json|http-client|http-server|http2|websocket|all) ;;
  *) echo "unknown suite: $suite" >&2; exit 2 ;;
esac

if ((${#patch_dirs[@]} == 0 && ${#patches[@]} == 0)); then
  case "$suite" in
    json) patch_dirs=(research/patches/json) ;;
    http-client) patch_dirs=(research/patches/http_client_1.1) ;;
    http-server) patch_dirs=(research/patches/http_server_1.1) ;;
    http2) patch_dirs=(research/patches/http_server_2.0) ;;
    websocket) patch_dirs=(research/patches/websocket) ;;
    auto|all) patch_dirs=(research/patches) ;;
  esac
fi

if ((${#patches[@]} == 0)); then
  for dir in "${patch_dirs[@]}"; do
    [[ -d "$dir" ]] || continue
    while IFS= read -r -d '' patch; do
      patches+=("$patch")
    done < <(find "$dir" -type f -name '*.patch' -print0 | sort -z)
  done
fi

((${#patches[@]} > 0)) || { echo "no patches found" >&2; exit 2; }

if ! git diff --quiet --no-ext-diff || ! git diff --cached --quiet --no-ext-diff; then
  echo "tracked or staged changes are present; refusing to run a patch/revert sweep" >&2
  exit 1
fi

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
report="${PERF_PATCH_REPORT:-findings/perf_patch_sweeps/$stamp/report.md}"
artifact_root="${PERF_PATCH_ARTIFACTS:-findings/perf_patch_sweeps/$stamp/artifacts}"
log_root="$artifact_root/logs"
bin_root="$artifact_root/bins"
mkdir -p "$(dirname "$report")" "$log_root" "$bin_root"

presets=(${PERF_PATCH_PRESETS:-release-clang-libcxx release-gcc16-stdcxx})
reps="${PERF_PATCH_REPS:-5}"
pguri="${PERF_PATCH_PGURI:-postgresql:///conflux_bench?user=postgres}"
current_patch=""

slugify() {
  tr '/ .' '---' <<< "$1" | tr -cd 'A-Za-z0-9._-'
}

suite_for_patch() {
  local patch="$1"
  case "$patch" in
    *research/patches/json/*|*json*) printf '%s\n' json ;;
    *research/patches/http_client_1.1/*|*http_client*|*client-h1*) printf '%s\n' http-client ;;
    *research/patches/http_server_1.1/*|*http11*|*http1*) printf '%s\n' http-server ;;
    *research/patches/http_server_2.0/*|*h2*) printf '%s\n' http2 ;;
    *research/patches/websocket/*|*websocket*|*ws*) printf '%s\n' websocket ;;
    *) printf '%s\n' unknown ;;
  esac
}

patch_enabled_for_suite() {
  local patch="$1" wanted="$2" actual
  [[ "$wanted" == all ]] && return 0
  actual="$(suite_for_patch "$patch")"
  [[ "$wanted" == auto ]] && [[ "$actual" != unknown ]] && return 0
  [[ "$wanted" == "$actual" ]]
}

targets_for_patch() {
  local patch="$1" wanted="$2" actual
  if [[ -n "${PERF_PATCH_TARGETS:-}" ]]; then
    printf '%s\n' ${PERF_PATCH_TARGETS}
    return 0
  fi
  actual="$(suite_for_patch "$patch")"
  [[ "$wanted" == auto || "$wanted" == all ]] || actual="$wanted"
  case "$actual" in
    json)
      printf '%s\n' conflux_json_bench conflux_json_storage_bench
      ;;
    http-server)
      printf '%s\n' conflux_http_parser_bench conflux_http_app_path_bench conflux_http_adversarial_bench conflux_http_server_bench conflux_http_server_concurrency_bench
      ;;
    http2)
      printf '%s\n' conflux_http_server_bench conflux_http_server_concurrency_bench
      ;;
    websocket)
      printf '%s\n' conflux_http_server_bench conflux_slow_consumer_backpressure_bench conflux_cpu_dispatch_impl_bench
      ;;
  esac
}

benches_for_patch() {
  local patch="$1" wanted="$2" actual
  if [[ -n "${PERF_PATCH_BENCHES:-}" ]]; then
    printf '%s\n' ${PERF_PATCH_BENCHES}
    return 0
  fi
  actual="$(suite_for_patch "$patch")"
  [[ "$wanted" == auto || "$wanted" == all ]] || actual="$wanted"
  case "$actual" in
    json)
      printf '%s\n' json json_storage
      ;;
    http-server)
      printf '%s\n' http_parser http_app_path http_adversarial http_server http_server_concurrency
      ;;
    http2)
      printf '%s\n' http_server http_server_concurrency
      ;;
    websocket)
      printf '%s\n' http_server slow_consumer_backpressure cpu_dispatch_impl
      ;;
  esac
}

build_dir_for_preset() {
  printf '/tmp/gcc-16/%s\n' "$1"
}

build_targets() {
  local preset="$1" label="$2"
  shift 2
  local -a targets=("$@")
  local log="$log_root/build.$label.$preset.log"
  echo "building $label $preset: ${targets[*]}"
  if cmake --build --preset "$preset" --target "${targets[@]}" >"$log" 2>&1; then
    return 0
  fi
  echo "build failed for $label $preset; log: $log" >&2
  return 1
}

copy_bench_bins() {
  local preset="$1" label="$2" dest="$3"
  local build_dir bench_dir
  build_dir="$(build_dir_for_preset "$preset")"
  bench_dir="$build_dir/benchmarks"
  mkdir -p "$dest"
  [[ -d "$bench_dir" ]] || return 0
  find "$bench_dir" -maxdepth 1 -type f -executable -name 'conflux_*bench*' -exec cp -p {} "$dest/" \;
}

append_report() {
  printf '%s\n' "$*" >>"$report"
}

revert_current_patch() {
  if [[ -n "$current_patch" ]]; then
    if git apply --reverse --check "$current_patch" >/dev/null 2>&1; then
      git apply --reverse "$current_patch" || true
    fi
    current_patch=""
  fi
}

trap revert_current_patch EXIT INT TERM

append_report "# Perf Patch Sweep"
append_report ""
append_report "- Started: $stamp"
append_report "- Presets: ${presets[*]}"
append_report "- Reps: $reps"
append_report "- Artifacts: $artifact_root"
append_report ""

declare -A baseline_ready=()
declare -A candidate_dirs=()
declare -A compare_benches=()
declare -A build_groups=()

for patch in "${patches[@]}"; do
  [[ -f "$patch" ]] || { append_report "## $patch"; append_report ""; append_report "Missing patch file."; append_report ""; continue; }
  if ! patch_enabled_for_suite "$patch" "$suite"; then
    continue
  fi
  mapfile -t targets < <(targets_for_patch "$patch" "$suite" | awk 'NF' | sort -u)
  mapfile -t benches < <(benches_for_patch "$patch" "$suite" | awk 'NF' | sort -u)
  patch_slug="$(slugify "$patch")"
  append_report "## $patch"
  append_report ""
  if ((${#targets[@]} == 0 || ${#benches[@]} == 0)); then
    append_report "No benchmark suite inferred. Use PERF_PATCH_TARGETS and PERF_PATCH_BENCHES or --suite."
    append_report ""
    continue
  fi
  append_report "- Targets: ${targets[*]}"
  append_report "- Benches: ${benches[*]}"
  build_key="${targets[*]}"
  build_slug="$(slugify "$build_key")"
  build_groups["$build_key"]=1
  for bench in "${benches[@]}"; do
    compare_benches["$bench"]=1
  done

  if ! git apply --check "$patch" >"$log_root/apply-check.$patch_slug.log" 2>&1; then
    append_report "- Apply: failed; see $log_root/apply-check.$patch_slug.log"
    append_report ""
    continue
  fi

  for preset in "${presets[@]}"; do
    if [[ -z "${baseline_ready[$preset|$build_key]:-}" ]]; then
      if build_targets "$preset" baseline "${targets[@]}"; then
        copy_bench_bins "$preset" baseline "$bin_root/baseline/$preset/$build_slug"
        baseline_ready[$preset|$build_key]="$bin_root/baseline/$preset/$build_slug"
      else
        append_report "- Baseline $preset: build failed; candidates for this target set skipped."
        baseline_ready[$preset|$build_key]=failed
      fi
    fi
  done

  git apply "$patch"
  current_patch="$patch"
  append_report "- Apply: ok"

  for preset in "${presets[@]}"; do
    if [[ "${baseline_ready[$preset|$build_key]:-}" == failed ]]; then
      continue
    fi
    cand_dir="$bin_root/$patch_slug/$preset"
    if build_targets "$preset" "$patch_slug" "${targets[@]}"; then
      copy_bench_bins "$preset" "$patch_slug" "$cand_dir"
      append_report "- Build $preset: ok"
      candidate_dirs["$preset|$build_key"]+="$patch_slug:$cand_dir"$'\n'
    else
      append_report "- Build $preset: failed; see $log_root/build.$patch_slug.$preset.log"
      continue
    fi
  done

  revert_current_patch
  if ! git diff --quiet --no-ext-diff || ! git diff --cached --quiet --no-ext-diff; then
    append_report "- Revert: dirty tracked tree remains; stopping."
    echo "patch revert left tracked changes; inspect before continuing" >&2
    exit 1
  fi
  append_report "- Revert: ok"
  append_report ""
done

summarize_compare() {
  local bench="$1" base_label="$2" suffix="$3" out="$4"
  psql "$pguri" -At -F ' | ' -q -v ON_ERROR_STOP=1 -c "
    WITH summary AS (
      SELECT runs.build_preset AS label,
             results.variant,
             results.best,
             results.p10,
             results.p50,
             results.p99
      FROM results
      JOIN runs ON runs.id = results.run_id
      WHERE runs.name = 'compare-bins'
        AND runs.benchmark = '$(printf '%s' "$bench" | sed "s/'/''/g")'
        AND runs.config_name = 'compare-bins'
        AND runs.created_at >= '$(printf '%s' "$stamp" | sed 's/\(........\)T\(......\)Z/\1 \2 UTC/' | sed 's/\(....\)\(..\)\(..\) \(..\)\(..\)\(..\)/\1-\2-\3 \4:\5:\6/')'::timestamptz
        AND results.extra->>'kind' = 'summary'
    ),
    base AS (
      SELECT variant, best, p10, p50, p99
      FROM summary
      WHERE label = '$(printf '%s' "$base_label" | sed "s/'/''/g")'
    )
    SELECT regexp_replace(summary.label, '$(printf '%s' "$suffix" | sed "s/'/''/g")$', '') AS patch,
           summary.variant,
           round(base.best::numeric, 2),
           round(((summary.best - base.best) / NULLIF(base.best, 0) * 100.0)::numeric, 2),
           round(base.p10::numeric, 2),
           round(((summary.p10 - base.p10) / NULLIF(base.p10, 0) * 100.0)::numeric, 2),
           round(base.p50::numeric, 2),
           round(((summary.p50 - base.p50) / NULLIF(base.p50, 0) * 100.0)::numeric, 2),
           round(base.p99::numeric, 2),
           round(((summary.p99 - base.p99) / NULLIF(base.p99, 0) * 100.0)::numeric, 2)
    FROM summary
    JOIN base ON base.variant = summary.variant
    WHERE summary.label <> '$(printf '%s' "$base_label" | sed "s/'/''/g")'
      AND summary.label LIKE '%$(printf '%s' "$suffix" | sed "s/'/''/g")'
    ORDER BY patch, summary.variant;" >>"$out"
}

append_report "## Grouped Compare Results"
append_report ""
append_report "Format: patch | variant | base_best_ns | best_pct | base_p10_ns | p10_pct | base_p50_ns | p50_pct | base_p99_ns | p99_pct"
append_report ""

for preset in "${presets[@]}"; do
  for build_key in "${!build_groups[@]}"; do
    base_dir="${baseline_ready[$preset|$build_key]:-}"
    [[ -n "$base_dir" && "$base_dir" != failed ]] || continue
    candidates="${candidate_dirs[$preset|$build_key]:-}"
    [[ -n "$candidates" ]] || continue
    for bench in "${!compare_benches[@]}"; do
      compare_log="$log_root/compare.$preset.$(slugify "$bench").log"
      args=(--yes --reps "$reps" --pguri "$pguri" --dir "base-$preset:$base_dir")
      while IFS= read -r candidate; do
        [[ -n "$candidate" ]] || continue
        label="${candidate%%:*}"
        dir="${candidate#*:}"
        args+=(--dir "$label-$preset:$dir")
      done <<< "$candidates"
      if scripts/compare_bins_by_bench.sh "${args[@]}" "$bench" >"$compare_log" 2>&1; then
        append_report "### $preset / $bench"
        append_report ""
        append_report '```text'
        summarize_compare "$bench" "base-$preset" "-$preset" "$report"
        append_report '```'
        append_report ""
        append_report "- Compare log: $compare_log"
        append_report ""
      else
        append_report "### $preset / $bench"
        append_report ""
        append_report "- Compare failed or skipped; see $compare_log"
        append_report ""
      fi
    done
  done
done

append_report "Completed: $(date -u +%Y%m%dT%H%M%SZ)"
printf 'report: %s\n' "$report"
