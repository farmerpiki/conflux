#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/json_perf_build_profiles.sh --tree LABEL:PATH [--tree LABEL:PATH ...]

Builds the JSON benchmark targets for each worktree and selected profile. The
default profiles cover normal release, O2/LTO release, and PGO-generate lanes;
PGO-use is built by json_perf_run_conditions.sh after profile data has been
generated.

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
allow_gcc15_lto_failure="${JSON_PERF_ALLOW_GCC15_LTO_FAILURE:-1}"
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
