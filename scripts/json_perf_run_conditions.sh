#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/json_perf_run_conditions.sh --tree LABEL:PATH [--tree LABEL:PATH ...]

Runs JSON benchmark candidates under:
  - normal release compare-bins, 5 reps
  - O2/LTO release compare-bins, 5 reps
  - PGO profile generation, 1 rep
  - PGO-use release compare-bins, 5 reps
  - perf-stat capture, 1 rep, for normal, O2/LTO, and PGO-use binaries

Environment:
  PGURI                 default: postgresql:///conflux_bench?user=postgres
  BENCH_PIN_CPUS        optional taskset cpuset passed through to bench_record
  JSON_PERF_REPS        default: 5
  JSON_PERF_BENCHES     default: json json_storage
  JSON_PERF_PROFILES    default: clang-libcxx gcc-stdcxx gcc16-stdcxx
  JSON_PERF_TARGETS     default: conflux_json_bench conflux_json_storage_bench
  JSON_PERF_PGO_ROOT    default: /tmp/conflux-json-pgo
  JSON_PERF_ARTIFACTS   default: /tmp/conflux-json-perf-artifacts/<timestamp>
  JSON_PERF_PGO_OPT_FLAGS
                        default: -O2 -DNDEBUG
  JSON_PERF_ALLOW_GCC15_LTO_FAILURE
                        skip missing gcc-stdcxx O2/LTO or PGO binaries,
                        default: 1
EOF
}

trees=()
while (($#)); do
  case "$1" in
    --tree)
      shift
      [[ $# -gt 0 && "$1" == *:* ]] || { echo "--tree needs LABEL:PATH" >&2; exit 2; }
      trees+=("$1")
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

if ((${#trees[@]} == 0)); then
  while IFS= read -r -d '' path; do
    label="${path##*/}"
    label="${label#conflux-jsonpatch-}"
    trees+=("$label:$path")
  done < <(find /tmp -maxdepth 1 -type d -name 'conflux-jsonpatch-*' -print0 | sort -z)
fi

((${#trees[@]} > 0)) || { echo "no worktrees provided or discovered" >&2; exit 2; }

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pguri="${PGURI:-postgresql:///conflux_bench?user=postgres}"
reps="${JSON_PERF_REPS:-5}"
benches=(${JSON_PERF_BENCHES:-json json_storage})
profiles=(${JSON_PERF_PROFILES:-clang-libcxx gcc-stdcxx gcc16-stdcxx})
targets=(${JSON_PERF_TARGETS:-conflux_json_bench conflux_json_storage_bench})
pgo_root="${JSON_PERF_PGO_ROOT:-/tmp/conflux-json-pgo}"
pgo_opt_flags="${JSON_PERF_PGO_OPT_FLAGS:--O2 -DNDEBUG}"
allow_gcc15_lto_failure="${JSON_PERF_ALLOW_GCC15_LTO_FAILURE:-1}"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
artifact_root="${JSON_PERF_ARTIFACTS:-/tmp/conflux-json-perf-artifacts/$stamp}"
mkdir -p "$artifact_root/logs" "$artifact_root/perf"

release_preset() { printf 'release-%s\n' "$1"; }
o2_lto_build_dir() { printf 'o2-lto-%s\n' "$1"; }
pgo_gen_preset() { printf 'pgo-gen-%s\n' "$1"; }
pgo_use_preset() { printf 'pgo-use-%s\n' "$1"; }

pgo_profile_dir() {
  local label="$1" profile="$2"
  printf '%s/%s/%s\n' "$pgo_root" "$label" "$profile"
}

pgo_config_args() {
  local label="$1" profile="$2" dir
  dir="$(pgo_profile_dir "$label" "$profile")"
  case "$profile" in
    clang-*)
      printf '%s\0' \
        "-DCONFLUX_PGO_PROFILE_DIR=$dir/merged.profdata" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON"
      ;;
    *)
      printf '%s\0' \
        "-DCONFLUX_PGO_PROFILE_DIR=$dir" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON" \
        "-DCONFLUX_LTO_MODE=AUTO"
      ;;
  esac
}

bench_bin_name() {
  case "$1" in
    json) printf '%s\n' conflux_json_bench ;;
    json_storage) printf '%s\n' conflux_json_storage_bench ;;
    *) echo "unknown json bench: $1" >&2; exit 2 ;;
  esac
}

may_skip_missing_binary() {
  local name="$1"
  [[ "$allow_gcc15_lto_failure" == 1 ]] || return 1
  case "$name" in
    o2-lto-gcc-stdcxx|pgo-gen-gcc-stdcxx|pgo-use-gcc-stdcxx) return 0 ;;
    *) return 1 ;;
  esac
}

append_existing_dirs() {
  local condition="$1" name="$2" build_dir_name="$3" bench="$4" bin_name
  APPENDED_DIRS=0
  bin_name="$(bench_bin_name "$bench")"
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    src="${spec#*:}"
    bin="$src/build/$build_dir_name/benchmarks/$bin_name"
    if [[ -x "$bin" ]]; then
      args+=(--dir "$condition-$name-$label:$src/build/$build_dir_name")
      APPENDED_DIRS=$((APPENDED_DIRS + 1))
    elif may_skip_missing_binary "$name"; then
      echo "WARN: missing $condition $name $bench binary for $label; skipping this candidate" >&2
    else
      echo "missing benchmark binary: $bin" >&2
      exit 1
    fi
  done
}

compare_one() {
  local condition="$1" name="$2" build_dir_name="$3" bench="$4" baseline_run_id="${5:-}" log
  local -a args
  log="$artifact_root/logs/${condition}.${name}.${bench}.log"
  echo "==> compare $condition $name $bench"
  args=(--yes --reps "$reps" --pguri "$pguri")
  [[ -n "$baseline_run_id" ]] && args+=(--baseline-run-id "$baseline_run_id")
  append_existing_dirs "$condition" "$name" "$build_dir_name" "$bench"
  if ((APPENDED_DIRS < 2)); then
    echo "WARN: fewer than two candidates for $condition $name $bench; skipping compare" >&2
    COMPARE_RUN_ID=""
    return 0
  fi
  scripts/compare_bins_by_bench.sh "${args[@]}" "$bench" | tee "$log"
}

calibrate_one() {
  local name="$1" build_dir_name="$2" bench="$3" log rid
  local -a args
  log="$artifact_root/logs/calibrate.${name}.${bench}.log"
  echo "==> calibrate $name $bench"
  args=(--yes --reps 1 --pguri "$pguri")
  append_existing_dirs calibrate "$name" "$build_dir_name" "$bench"
  if ((APPENDED_DIRS < 2)); then
    echo "WARN: fewer than two candidates for calibration $name $bench; skipping" >&2
    CALIBRATION_RUN_ID=""
    return 0
  fi
  scripts/compare_bins_by_bench.sh "${args[@]}" "$bench" | tee "$log"
  rid="$(awk '/run_id=/{ sub(/^.*run_id=/, ""); print; exit }' "$log")"
  [[ -n "$rid" ]] || { echo "could not extract calibration run id from $log" >&2; exit 1; }
  CALIBRATION_RUN_ID="$rid"
}

generate_pgo_profiles() {
  local profile="$1" bench="$2" baseline_run_id="$3" preset args=()
  preset="$(pgo_gen_preset "$profile")"
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    mkdir -p "$(pgo_profile_dir "$label" "$profile")"
  done
  echo "==> generate PGO profiles $profile $bench"
  args=(--yes --reps 1 --pguri "$pguri" --baseline-run-id "$baseline_run_id")
  appended_dirs=0
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    src="${spec#*:}"
    bin="$src/build/$preset/benchmarks/$(bench_bin_name "$bench")"
    if [[ -x "$bin" ]]; then
      args+=(--dir "pgo-gen-$profile-$label:$src/build/$preset")
      appended_dirs=$((appended_dirs + 1))
    elif may_skip_missing_binary "$preset"; then
      echo "WARN: missing pgo-gen $profile $bench binary for $label; skipping this candidate" >&2
    else
      echo "missing benchmark binary: $bin" >&2
      exit 1
    fi
  done
  if ((appended_dirs < 2)); then
    echo "WARN: fewer than two candidates for pgo-gen $profile $bench; skipping" >&2
    return 0
  fi
  BENCH_REPS=1 scripts/compare_bins_by_bench.sh "${args[@]}" "$bench" \
    | tee "$artifact_root/logs/pgo-gen.${profile}.${bench}.log"
}

merge_clang_profiles() {
  local profile="$1"
  [[ "$profile" == clang-* ]] || return 0
  command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found" >&2; exit 2; }
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    dir="$(pgo_profile_dir "$label" "$profile")"
    find "$dir" -name '*.profraw' -type f -print -quit | grep -q . || {
      echo "no clang profraw files in $dir" >&2
      exit 1
    }
    llvm-profdata merge -output="$dir/merged.profdata" "$dir"/*.profraw
  done
}

build_pgo_use() {
  local profile="$1" preset
  preset="$(pgo_use_preset "$profile")"
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    src="${spec#*:}"
    build="$src/build/$preset"
    declare -a override_args=()
    while IFS= read -r -d '' arg; do
      override_args+=("$arg")
    done < <(pgo_config_args "$label" "$profile")
    echo "==> configure $label $preset"
    cmake --preset "$preset" -S "$src" -B "$build" "${override_args[@]}"
    echo "==> build $label $preset: ${targets[*]}"
    if cmake --build "$build" --target "${targets[@]}"; then
      continue
    fi
    if may_skip_missing_binary "$preset"; then
      echo "WARN: $label $preset build failed; continuing because gcc15 LTO/PGO failures are non-blocking" >&2
      continue
    fi
    return 1
  done
}

baseline_iterations() {
  local run_id="$1"
  psql "$pguri" -Atc "select max(iterations) from results where run_id = $run_id"
}

perf_capture() {
  local condition="$1" name="$2" build_dir_name="$3" bench="$4" baseline_run_id="$5" bin_name iter
  local -a cmd
  bin_name="$(bench_bin_name "$bench")"
  iter="$(baseline_iterations "$baseline_run_id")"
  [[ -n "$iter" && "$iter" != "0" ]] || { echo "no iterations found for run_id=$baseline_run_id" >&2; exit 1; }
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    src="${spec#*:}"
    bin="$src/build/$build_dir_name/benchmarks/$bin_name"
    if [[ ! -x "$bin" ]]; then
      if may_skip_missing_binary "$name"; then
        echo "WARN: missing perf binary for $condition $name $bench $label; skipping" >&2
        continue
      fi
      echo "missing benchmark binary: $bin" >&2
      exit 1
    fi
    out="$artifact_root/perf/${condition}.${name}.${bench}.${label}.ndjson"
    perf_json="$artifact_root/perf/${condition}.${name}.${bench}.${label}.perf.json"
    perf_stderr="$artifact_root/perf/${condition}.${name}.${bench}.${label}.perf.stderr"
    echo "==> perf $condition $name $bench $label iterations=$iter"
    cmd=("$bin" --iterations "$iter" --json)
    if [[ -n "${BENCH_PIN_CPUS:-}" ]]; then
      cmd=(taskset -c "$BENCH_PIN_CPUS" "${cmd[@]}")
    fi
    python3 scripts/bench_perf_stat.py \
      --output "$out" \
      --perf-json "$perf_json" \
      --perf-stderr "$perf_stderr" \
      -- "${cmd[@]}"
  done
}

cd "$repo_root"

declare -A calibration_ids=()
for profile in "${profiles[@]}"; do
  preset="$(release_preset "$profile")"
  o2_build="$(o2_lto_build_dir "$profile")"
  for bench in "${benches[@]}"; do
    calibrate_one "$preset" "$preset" "$bench"
    rid="$CALIBRATION_RUN_ID"
    [[ -n "$rid" ]] || continue
    calibration_ids["normal/$profile/$bench"]="$rid"
    compare_one normal "$preset" "$preset" "$bench" "$rid"
    perf_capture normal "$preset" "$preset" "$bench" "$rid"

    calibrate_one "$o2_build" "$o2_build" "$bench"
    rid="$CALIBRATION_RUN_ID"
    [[ -n "$rid" ]] || continue
    calibration_ids["o2-lto/$profile/$bench"]="$rid"
    compare_one o2-lto "$o2_build" "$o2_build" "$bench" "$rid"
    perf_capture o2-lto "$o2_build" "$o2_build" "$bench" "$rid"
  done
done

for profile in "${profiles[@]}"; do
  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    rm -rf "$(pgo_profile_dir "$label" "$profile")"
    mkdir -p "$(pgo_profile_dir "$label" "$profile")"
  done
  for bench in "${benches[@]}"; do
    rid="${calibration_ids["o2-lto/$profile/$bench"]:-}"
    [[ -n "$rid" ]] || continue
    generate_pgo_profiles "$profile" "$bench" "$rid"
  done
  merge_clang_profiles "$profile"
  build_pgo_use "$profile"
  preset="$(pgo_use_preset "$profile")"
  for bench in "${benches[@]}"; do
    rid="${calibration_ids["o2-lto/$profile/$bench"]:-}"
    [[ -n "$rid" ]] || continue
    compare_one pgo "$preset" "$preset" "$bench" "$rid"
    perf_capture pgo "$preset" "$preset" "$bench" "$rid"
  done
done

echo "artifacts: $artifact_root"
