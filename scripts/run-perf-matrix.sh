#!/usr/bin/env bash
# Perf CI matrix: configure and build benchmark binaries from perf presets only.
#
# This is intentionally build/manifest work, not sanitizer correctness work and
# not a replacement for DB-backed benchmark recording. Use scripts/bench_record.sh
# for measured runs on a quiet host.
#
# Usage:
#   ./scripts/run-perf-matrix.sh [--only PRESET,...] [--target TARGET]
#
# Env:
#   SOURCE_DIR   path to project root (default: repo root via git)
#   JOBS         parallel jobs for cmake --build (default: nproc)
set -euo pipefail

MATRIX=(
    perf-clang-libcxx
    perf-gcc-stdcxx
)

ONLY=()
TARGET=conflux_record_benches

while (($# > 0)); do
    case "$1" in
        --only)
            shift
            IFS=',' read -ra ONLY <<< "$1"
            shift
            ;;
        --target)
            shift
            TARGET="$1"
            shift
            ;;
        --)
            shift
            break
            ;;
        *)
            printf 'unknown arg: %s\n' "$1" >&2
            printf 'usage: %s [--only PRESET,...] [--target TARGET]\n' "$0" >&2
            exit 2
            ;;
    esac
done

script_repo_root() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$script_dir/.." && pwd
}

if [[ -z "${SOURCE_DIR:-}" ]]; then
    SOURCE_DIR="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null || script_repo_root)"
fi
SOURCE_DIR="$(realpath "$SOURCE_DIR")"
JOBS="${JOBS:-$(nproc)}"

in_only() {
    local preset="$1"
    if ((${#ONLY[@]} == 0)); then return 0; fi
    local p
    for p in "${ONLY[@]}"; do
        [[ "$p" == "$preset" ]] && return 0
    done
    return 1
}

pad() { printf '%-28s' "$1"; }

cache_value() {
    local build_dir="$1" key="$2"
    sed -n "s/^${key}:[A-Z]*=//p" "$build_dir/CMakeCache.txt" | tail -1
}

assert_perf_cache() {
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
        printf '%s enables sanitizers; perf presets must not.\n' "$preset" >&2
        return 1
    fi
    if [[ "$benches" != ON ]]; then
        printf '%s does not build benchmark targets.\n' "$preset" >&2
        return 1
    fi
    if [[ "$tests" != OFF ]]; then
        printf '%s builds test targets; perf presets should stay benchmark-only.\n' "$preset" >&2
        return 1
    fi
    if [[ "$lto" != OFF ]]; then
        printf '%s enables LTO; perf presets should stay recordable/profile-friendly.\n' "$preset" >&2
        return 1
    fi
    if [[ "$build_type" != RelWithDebInfo ]]; then
        printf '%s uses CMAKE_BUILD_TYPE=%s; perf presets use RelWithDebInfo.\n' "$preset" "$build_type" >&2
        return 1
    fi
}

declare -A RESULTS=()
selected=0

for preset in "${MATRIX[@]}"; do
    in_only "$preset" || continue
    selected=$((selected + 1))

    printf '\n━━━ %s ━━━\n' "$preset"
    build_dir="/tmp/$(basename "$SOURCE_DIR")/$preset"
    status=PASS

    if ! cmake --preset "$preset" -S "$SOURCE_DIR" 2>&1; then
        status=CONFIGURE_FAIL
    fi

    if [[ "$status" == PASS ]]; then
        if ! assert_perf_cache "$preset" "$build_dir"; then
            status=PRESET_SHAPE_FAIL
        fi
    fi

    if [[ "$status" == PASS ]]; then
        if ! cmake --build --preset "$preset" --target "$TARGET" -j "$JOBS" 2>&1; then
            status=BUILD_FAIL
        fi
    fi

    RESULTS[$preset]=$status
done

if ((selected == 0)); then
    printf 'no perf presets selected by --only filter.\n' >&2
    exit 2
fi

printf '\n━━━ Perf Matrix Results ━━━\n'
overall=0
for preset in "${MATRIX[@]}"; do
    in_only "$preset" || continue
    result="${RESULTS[$preset]}"
    if [[ "$result" == PASS ]]; then
        printf '%s  ✓ %s\n' "$(pad "$preset")" "$result"
    else
        printf '%s  ✗ %s\n' "$(pad "$preset")" "$result"
        overall=1
    fi
done
printf '\n'

if ((overall == 0)); then
    printf 'Perf matrix built benchmark targets only, with sanitizers and tests disabled.\n'
    printf 'Use scripts/bench_record.sh for measured DB-backed runs on a quiet host.\n\n'
fi

exit $overall
