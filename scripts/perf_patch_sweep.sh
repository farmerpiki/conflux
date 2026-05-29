#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/perf_patch_sweep.sh [--patch PATH ...] [--patch-dir DIR ...] [--suite auto|json|http|ws|all]

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
patch_dirs=(research/patches)
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
  auto|json|http|ws|all) ;;
  *) echo "unknown suite: $suite" >&2; exit 2 ;;
esac

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

targets_for_patch() {
  local patch="$1" wanted="$2"
  if [[ -n "${PERF_PATCH_TARGETS:-}" ]]; then
    printf '%s\n' ${PERF_PATCH_TARGETS}
    return 0
  fi
  case "$wanted:$patch" in
    json:*|auto:*json*|all:*)
      printf '%s\n' conflux_json_bench conflux_json_storage_bench
      ;;
  esac
  case "$wanted:$patch" in
    http:*|auto:*http_server*|auto:*http_client*|auto:*http11*|auto:*h2*|all:*)
      printf '%s\n' conflux_http_parser_bench conflux_http_app_path_bench conflux_http_adversarial_bench conflux_http_server_bench conflux_http_server_concurrency_bench
      ;;
  esac
  case "$wanted:$patch" in
    ws:*|auto:*websocket*|auto:*ws*|all:*)
      printf '%s\n' conflux_http_server_bench conflux_slow_consumer_backpressure_bench conflux_cpu_dispatch_impl_bench
      ;;
  esac
}

benches_for_patch() {
  local patch="$1" wanted="$2"
  if [[ -n "${PERF_PATCH_BENCHES:-}" ]]; then
    printf '%s\n' ${PERF_PATCH_BENCHES}
    return 0
  fi
  case "$wanted:$patch" in
    json:*|auto:*json*|all:*)
      printf '%s\n' json json_storage
      ;;
  esac
  case "$wanted:$patch" in
    http:*|auto:*http_server*|auto:*http_client*|auto:*http11*|auto:*h2*|all:*)
      printf '%s\n' http_parser http_app_path http_adversarial http_server http_server_concurrency
      ;;
  esac
  case "$wanted:$patch" in
    ws:*|auto:*websocket*|auto:*ws*|all:*)
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

for patch in "${patches[@]}"; do
  [[ -f "$patch" ]] || { append_report "## $patch"; append_report ""; append_report "Missing patch file."; append_report ""; continue; }
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

  if ! git apply --check "$patch" >"$log_root/apply-check.$patch_slug.log" 2>&1; then
    append_report "- Apply: failed; see $log_root/apply-check.$patch_slug.log"
    append_report ""
    continue
  fi

  for preset in "${presets[@]}"; do
    if [[ -z "${baseline_ready[$preset|${targets[*]}]:-}" ]]; then
      if build_targets "$preset" baseline "${targets[@]}"; then
        copy_bench_bins "$preset" baseline "$bin_root/baseline/$preset"
        baseline_ready[$preset|${targets[*]}]=1
      else
        append_report "- Baseline $preset: build failed; candidates for this target set skipped."
        baseline_ready[$preset|${targets[*]}]=failed
      fi
    fi
  done

  git apply "$patch"
  current_patch="$patch"
  append_report "- Apply: ok"

  for preset in "${presets[@]}"; do
    if [[ "${baseline_ready[$preset|${targets[*]}]:-}" == failed ]]; then
      continue
    fi
    cand_dir="$bin_root/$patch_slug/$preset"
    if build_targets "$preset" "$patch_slug" "${targets[@]}"; then
      copy_bench_bins "$preset" "$patch_slug" "$cand_dir"
      append_report "- Build $preset: ok"
    else
      append_report "- Build $preset: failed; see $log_root/build.$patch_slug.$preset.log"
      continue
    fi
    for bench in "${benches[@]}"; do
      compare_log="$log_root/compare.$patch_slug.$preset.$bench.log"
      if scripts/compare_bins_by_bench.sh \
          --yes \
          --reps "$reps" \
          --pguri "$pguri" \
          --dir "base:$bin_root/baseline/$preset" \
          --dir "candidate:$cand_dir" \
          "$bench" >"$compare_log" 2>&1; then
        append_report "- Compare $preset $bench: ok; see $compare_log"
      else
        append_report "- Compare $preset $bench: failed or skipped; see $compare_log"
      fi
    done
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

append_report "Completed: $(date -u +%Y%m%dT%H%M%SZ)"
printf 'report: %s\n' "$report"
