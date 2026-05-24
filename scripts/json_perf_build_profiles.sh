#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/json_perf_build_profiles.sh --tree LABEL:PATH [--tree LABEL:PATH ...]

Builds the JSON benchmark targets for each worktree and selected profile. The
default profiles cover normal release, O2/LTO release, PGO-generate lanes, and
PGO-use lanes. PGO-generate binaries are calibrated with --iterations 0 first,
then rerun with the discovered iteration count before PGO-use builds so benchmark
measurement can run later without mixed build/profile work.

Environment:
  JSON_PERF_PROFILES   space-separated presets
                       default: release-clang-libcxx release-gcc-stdcxx release-gcc16-stdcxx
                                pgo-gen-clang-libcxx pgo-gen-gcc-stdcxx pgo-gen-gcc16-stdcxx
  JSON_PERF_TARGETS    space-separated targets
                       default: conflux_json_bench conflux_json_storage_bench
  JSON_PERF_PGO_ROOT   profile-data root for generated profiles
                       default: /tmp/conflux-json-pgo
  JSON_PERF_PGO_OPT_FLAGS
                       default: -O2 -DNDEBUG
  JSON_PERF_BUILD_O2_LTO
                       build extra release-O2/LTO trees, default: 1
  JSON_PERF_BUILD_PGO_USE
                       run PGO generation and build PGO-use trees, default: 1
  JSON_PERF_PGO_ITERATIONS
                       fixed iterations for PGO training; default: discover with 0
  JSON_PERF_ALLOW_GCC15_LTO_FAILURE
                       continue if gcc-stdcxx O2/LTO or PGO builds fail,
                       default: 1
EOF
}

profiles=(${JSON_PERF_PROFILES:-release-clang-libcxx release-gcc-stdcxx release-gcc16-stdcxx pgo-gen-clang-libcxx pgo-gen-gcc-stdcxx pgo-gen-gcc16-stdcxx})
targets=(${JSON_PERF_TARGETS:-conflux_json_bench conflux_json_storage_bench})
pgo_root="${JSON_PERF_PGO_ROOT:-/tmp/conflux-json-pgo}"
pgo_opt_flags="${JSON_PERF_PGO_OPT_FLAGS:--O2 -DNDEBUG}"
build_o2_lto="${JSON_PERF_BUILD_O2_LTO:-1}"
build_pgo_use="${JSON_PERF_BUILD_PGO_USE:-1}"
pgo_iterations="${JSON_PERF_PGO_ITERATIONS:-}"
allow_gcc15_lto_failure="${JSON_PERF_ALLOW_GCC15_LTO_FAILURE:-1}"
trees=()
declare -A pgo_training_iterations=()

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

pgo_config_args() {
  local label="$1" profile="$2" dir
  case "$profile" in
    pgo-gen-clang-*)
      dir="$pgo_root/$label/${profile#pgo-gen-}"
      printf '%s\0' \
        "-DCONFLUX_PGO_PROFILE_DIR=$dir/%m-%p.profraw" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON"
      ;;
    pgo-gen-gcc-*|pgo-gen-gcc16-*)
      dir="$pgo_root/$label/${profile#pgo-gen-}"
      printf '%s\0' \
        "-DCONFLUX_PGO_PROFILE_DIR=$dir" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON" \
        "-DCONFLUX_LTO_MODE=AUTO"
      ;;
    *)
      return 0
      ;;
  esac
}

pgo_use_config_args() {
  local label="$1" profile="$2" compiler_profile dir
  compiler_profile="${profile#pgo-use-}"
  dir="$pgo_root/$label/$compiler_profile"
  case "$profile" in
    pgo-use-clang-*)
      printf '%s\0' \
        "-DCONFLUX_PGO_GENERATE=OFF" \
        "-DCONFLUX_PGO_PROFILE_DIR=$dir/merged.profdata" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON"
      ;;
    pgo-use-gcc-*|pgo-use-gcc16-*)
      printf '%s\0' \
        "-DCONFLUX_PGO_GENERATE=OFF" \
        "-DCONFLUX_PGO_PROFILE_DIR=$dir" \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON" \
        "-DCONFLUX_LTO_MODE=AUTO"
      ;;
    *)
      return 0
      ;;
  esac
}

release_o2_lto_args() {
  local profile="$1"
  case "$profile" in
    release-*)
      printf '%s\0' \
        "-DCMAKE_CXX_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCMAKE_C_FLAGS_RELEASE=$pgo_opt_flags" \
        "-DCONFLUX_ENABLE_LTO=ON"
      case "$profile" in
        release-gcc-*|release-gcc16-*) printf '%s\0' "-DCONFLUX_LTO_MODE=AUTO" ;;
      esac
      ;;
  esac
}

o2_build_dir_name() {
  printf 'o2-lto-%s\n' "${1#release-}"
}

may_skip_build_failure() {
  local profile="$1"
  [[ "$allow_gcc15_lto_failure" == 1 ]] || return 1
  case "$profile" in
    o2-lto-gcc-stdcxx|pgo-gen-gcc-stdcxx|pgo-use-gcc-stdcxx) return 0 ;;
    *) return 1 ;;
  esac
}

build_targets() {
  local label="$1" profile="$2" build="$3"
  echo "==> build $label $profile: ${targets[*]}"
  if cmake --build "$build" --target "${targets[@]}"; then
    return 0
  fi
  if may_skip_build_failure "$profile"; then
    echo "WARN: $label $profile build failed; continuing because gcc15 LTO/PGO failures are non-blocking" >&2
    return 0
  fi
  return 1
}

bench_name_for_target() {
  case "$1" in
    conflux_json_bench) printf '%s\n' json ;;
    conflux_json_storage_bench) printf '%s\n' json_storage ;;
    *) echo "unknown JSON benchmark target: $1" >&2; exit 2 ;;
  esac
}

extract_max_iterations() {
  awk '
    {
      if (match($0, /"iterations":[[:space:]]*[0-9]+/)) {
        value = substr($0, RSTART, RLENGTH)
        sub(/^"iterations":[[:space:]]*/, "", value)
        if (value + 0 > max) max = value + 0
      }
    }
    END { if (max > 0) print max }
  ' "$1"
}

bench_info_iterations() {
  local bin="$1"
  "$bin" --bench-info \
    | jq -r '
      [
        .configs[]? | (.args // []) as $args |
        [range(0; ($args | length) - 1) |
          select($args[.] == "--iterations") |
          ($args[. + 1] | tonumber)]
      ]
      | flatten
      | max // empty
    '
}

calibrate_pgo_iterations() {
  local profile="$1" target="$2" compiler_profile bin bench log_dir log iter fixed_iter max_iter=0
  compiler_profile="${profile#pgo-gen-}"
  bench="$(bench_name_for_target "$target")"

  if [[ -n "$pgo_iterations" ]]; then
    pgo_training_iterations["$profile|$target"]="$pgo_iterations"
    return 0
  fi

  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    src="${spec#*:}"
    bin="$src/build/$profile/benchmarks/$target"
    if [[ ! -x "$bin" ]]; then
      if may_skip_build_failure "$profile"; then
        echo "WARN: missing $label $profile $target; skipping PGO calibration for this binary" >&2
        continue
      fi
      echo "missing PGO-generate binary: $bin" >&2
      exit 1
    fi
    fixed_iter="$(bench_info_iterations "$bin")"
    if [[ -n "$fixed_iter" && "$fixed_iter" != "0" ]]; then
      ((fixed_iter > max_iter)) && max_iter="$fixed_iter"
      continue
    fi
    log_dir="$src/build/pgo-gen-logs"
    mkdir -p "$log_dir"
    log="$log_dir/$compiler_profile.$bench.calibrate.ndjson"
    echo "==> calibrate PGO $label $compiler_profile $bench iterations=0"
    "$bin" --iterations 0 --json >"$log"
    iter="$(extract_max_iterations "$log")"
    [[ -n "$iter" ]] || { echo "could not discover PGO iterations from $log" >&2; exit 1; }
    ((iter > max_iter)) && max_iter="$iter"
  done

  if ((max_iter <= 0)); then
    if may_skip_build_failure "$profile"; then
      echo "WARN: no PGO calibration data for $profile $target; skipping" >&2
      return 0
    fi
    echo "no PGO calibration data for $profile $target" >&2
    exit 1
  fi
  pgo_training_iterations["$profile|$target"]="$max_iter"
}

run_pgo_generation() {
  local label="$1" src="$2" profile="$3" compiler_profile dir build target bin bench log_dir iter
  compiler_profile="${profile#pgo-gen-}"
  dir="$pgo_root/$label/$compiler_profile"
  build="$src/build/$profile"
  log_dir="$src/build/pgo-gen-logs"
  rm -rf "$dir"
  mkdir -p "$dir" "$log_dir"

  for target in "${targets[@]}"; do
    bin="$build/benchmarks/$target"
    if [[ ! -x "$bin" ]]; then
      if may_skip_build_failure "$profile"; then
        echo "WARN: missing $label $profile $target; skipping PGO generation for this binary" >&2
        continue
      fi
      echo "missing PGO-generate binary: $bin" >&2
      exit 1
    fi
    bench="$(bench_name_for_target "$target")"
    iter="${pgo_training_iterations["$profile|$target"]:-}"
    [[ -n "$iter" ]] || { echo "missing PGO training iterations for $profile $target" >&2; exit 1; }
    echo "==> generate PGO $label $compiler_profile $bench iterations=$iter"
    "$bin" --iterations "$iter" --json >"$log_dir/$compiler_profile.$bench.train.ndjson"
  done
}

merge_clang_profile() {
  local label="$1" profile="$2" dir
  [[ "$profile" == pgo-gen-clang-* ]] || return 0
  command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found" >&2; exit 2; }
  dir="$pgo_root/$label/${profile#pgo-gen-}"
  find "$dir" -name '*.profraw' -type f -print -quit | grep -q . || {
    echo "no clang profraw files in $dir" >&2
    exit 1
  }
  llvm-profdata merge -output="$dir/merged.profdata" "$dir"/*.profraw
}

build_pgo_use_profile() {
  local label="$1" src="$2" gen_profile="$3" use_profile build
  use_profile="pgo-use-${gen_profile#pgo-gen-}"
  case "$use_profile" in
    pgo-use-gcc-*|pgo-use-gcc16-*)
      # GCC encodes the object path into .gcda names. Reusing the generate
      # build directory lets -fprofile-use find the profiles it just wrote.
      build="$src/build/$gen_profile"
      ;;
    *)
      build="$src/build/$use_profile"
      ;;
  esac
  declare -a override_args=()
  while IFS= read -r -d '' arg; do
    override_args+=("$arg")
  done < <(pgo_use_config_args "$label" "$use_profile")
  echo "==> configure $label $use_profile"
  cmake --preset "$use_profile" -S "$src" -B "$build" "${override_args[@]}"
  build_targets "$label" "$use_profile" "$build"
  if [[ "$build" != "$src/build/$use_profile" ]]; then
    rm -rf "$src/build/$use_profile"
    ln -s "${gen_profile}" "$src/build/$use_profile"
  fi
}

for spec in "${trees[@]}"; do
  label="${spec%%:*}"
  src="${spec#*:}"
  [[ -d "$src" ]] || { echo "worktree not found for $label: $src" >&2; exit 1; }

  for profile in "${profiles[@]}"; do
    build="$src/build/$profile"
    declare -a override_args=()
    while IFS= read -r -d '' arg; do
      override_args+=("$arg")
    done < <(pgo_config_args "$label" "$profile")
    echo "==> configure $label $profile"
    if ((${#override_args[@]} > 0)); then
      cmake --preset "$profile" -S "$src" -B "$build" "${override_args[@]}"
    else
      cmake --preset "$profile" -S "$src" -B "$build"
    fi

    build_targets "$label" "$profile" "$build"

    if [[ "$build_o2_lto" == 1 && "$profile" == release-* ]]; then
      build="$src/build/$(o2_build_dir_name "$profile")"
      override_args=()
      while IFS= read -r -d '' arg; do
        override_args+=("$arg")
      done < <(release_o2_lto_args "$profile")
      echo "==> configure $label $(o2_build_dir_name "$profile") from $profile"
      cmake --preset "$profile" -S "$src" -B "$build" "${override_args[@]}"
      build_targets "$label" "$(o2_build_dir_name "$profile")" "$build"
    fi
  done
done

if [[ "$build_pgo_use" == 1 ]]; then
  for profile in "${profiles[@]}"; do
    [[ "$profile" == pgo-gen-* ]] || continue
    for target in "${targets[@]}"; do
      calibrate_pgo_iterations "$profile" "$target"
    done
  done

  for spec in "${trees[@]}"; do
    label="${spec%%:*}"
    src="${spec#*:}"
    for profile in "${profiles[@]}"; do
      [[ "$profile" == pgo-gen-* ]] || continue
      run_pgo_generation "$label" "$src" "$profile"
      merge_clang_profile "$label" "$profile"
      build_pgo_use_profile "$label" "$src" "$profile"
    done
  done
fi
