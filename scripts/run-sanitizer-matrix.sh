#!/usr/bin/env bash
# Sanitizer CI matrix: build + test correctness presets only.
#
# Benchmark targets are intentionally disabled in these presets. Use
# scripts/run-perf-matrix.sh or scripts/bench_record.sh for perf work.
#
# Presets covered:
#   debug-clang-libcxx   — clang, ASan+UBSan, no-recover
#   debug-gcc-stdcxx     — gcc,   no sanitizers (GCC 15 ICE: cp/module.cc:10037
#                          with ASan/UBSan + C++26 modules; compile correctness only)
#   tsan-clang-libcxx    — clang, TSan, RelWithDebInfo
#   tsan-gcc-stdcxx      — gcc,   TSan, RelWithDebInfo
#
# All four must be green before any correctness-sensitive item lands.
#
# Usage:
#   ./scripts/run-sanitizer-matrix.sh [--only PRESET,...] [-- <ctest-args>]
#
# Env:
#   SOURCE_DIR   path to project root (default: repo root via git)
set -euo pipefail

MATRIX=(
    debug-clang-libcxx
    debug-gcc-stdcxx
    tsan-clang-libcxx
    tsan-gcc-stdcxx
)

ONLY=()
CTEST_EXTRA=()

while (($# > 0)); do
    case "$1" in
        --only)
            shift
            IFS=',' read -ra ONLY <<< "$1"
            shift
            ;;
        --)
            shift
            CTEST_EXTRA=("$@")
            break
            ;;
        *)
            printf 'unknown arg: %s\n' "$1" >&2
            printf 'usage: %s [--only PRESET,...] [-- <ctest-args>]\n' "$0" >&2
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

: "${PG_TEST_CONNINFO:=postgresql:///postgres?user=postgres}"
: "${PG_CONNINFO:=postgresql:///conflux_bench?user=postgres}"

# ── helpers ──────────────────────────────────────────────────────────────────

in_only() {
    local preset="$1"
    if ((${#ONLY[@]} == 0)); then return 0; fi
    local p
    for p in "${ONLY[@]}"; do
        [[ "$p" == "$preset" ]] && return 0
    done
    return 1
}

pad() { printf '%-30s' "$1"; }

cache_value() {
    local build_dir="$1" key="$2"
    sed -n "s/^${key}:[A-Z_]*=//p" "$build_dir/CMakeCache.txt" | tail -1
}

assert_sanitizer_cache() {
    local preset="$1" build_dir="$2"
    local asan ubsan tsan benches tests lto build_type
    asan="$(cache_value "$build_dir" CONFLUX_ENABLE_ASAN)"
    ubsan="$(cache_value "$build_dir" CONFLUX_ENABLE_UBSAN)"
    tsan="$(cache_value "$build_dir" CONFLUX_ENABLE_TSAN)"
    benches="$(cache_value "$build_dir" CONFLUX_BUILD_BENCHMARKS)"
    tests="$(cache_value "$build_dir" CONFLUX_BUILD_TESTS)"
    lto="$(cache_value "$build_dir" CONFLUX_ENABLE_LTO)"
    build_type="$(cache_value "$build_dir" CMAKE_BUILD_TYPE)"

    if [[ "$tests" != ON ]]; then
        printf '%s does not build tests; sanitizer/correctness presets must run tests.\n' "$preset" >&2
        return 1
    fi
    if [[ "$benches" != OFF ]]; then
        printf '%s builds benchmarks; sanitizer/correctness presets must not build perf artifacts.\n' "$preset" >&2
        return 1
    fi
    if [[ "$lto" != OFF ]]; then
        printf '%s enables LTO; sanitizer/correctness presets keep LTO disabled for clearer diagnostics.\n' "$preset" >&2
        return 1
    fi

    case "$preset" in
        debug-clang-libcxx)
            [[ "$build_type" == Debug && "$asan" == ON && "$ubsan" == ON && "$tsan" == OFF ]] || {
                printf '%s expected Debug + ASan+UBSan only, got type=%s asan=%s ubsan=%s tsan=%s.\n' \
                    "$preset" "$build_type" "$asan" "$ubsan" "$tsan" >&2
                return 1
            }
            ;;
        debug-gcc-stdcxx)
            [[ "$build_type" == Debug && "$asan" == OFF && "$ubsan" == OFF && "$tsan" == OFF ]] || {
                printf '%s expected Debug without sanitizers due GCC module ICE, got type=%s asan=%s ubsan=%s tsan=%s.\n' \
                    "$preset" "$build_type" "$asan" "$ubsan" "$tsan" >&2
                return 1
            }
            ;;
        tsan-*)
            [[ "$build_type" == RelWithDebInfo && "$asan" == OFF && "$ubsan" == OFF && "$tsan" == ON ]] || {
                printf '%s expected RelWithDebInfo + TSan only, got type=%s asan=%s ubsan=%s tsan=%s.\n' \
                    "$preset" "$build_type" "$asan" "$ubsan" "$tsan" >&2
                return 1
            }
            ;;
        *)
            printf 'no sanitizer preset-shape rule for %s.\n' "$preset" >&2
            return 1
            ;;
    esac
}

# ── run ──────────────────────────────────────────────────────────────────────

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
        if ! assert_sanitizer_cache "$preset" "$build_dir"; then
            status=PRESET_SHAPE_FAIL
        fi
    fi

    if [[ "$status" == PASS ]]; then
        if ! cmake --build --preset "$preset" 2>&1; then
            status=BUILD_FAIL
        fi
    fi

    if [[ "$status" == PASS ]]; then
        if ! env \
            PG_TEST_CONNINFO="$PG_TEST_CONNINFO" \
            PG_CONNINFO="$PG_CONNINFO" \
            ctest --test-dir "$build_dir" \
                  --output-on-failure \
                  "${CTEST_EXTRA[@]+"${CTEST_EXTRA[@]}"}" 2>&1; then
            status=TEST_FAIL
        fi
    fi

    RESULTS[$preset]=$status
done

# ── summary ──────────────────────────────────────────────────────────────────

if ((selected == 0)); then
    printf 'no sanitizer presets selected by --only filter.\n' >&2
    exit 2
fi

printf '\n━━━ Sanitizer Matrix Results ━━━\n'
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
    printf 'Note: debug-gcc-stdcxx runs without ASan/UBSan due to GCC 15 ICE\n'
    printf '      (cp/module.cc:10037 with sanitizers + C++26 modules).\n'
    printf '      ASan/UBSan coverage provided by debug-clang-libcxx.\n\n'
fi

exit $overall
