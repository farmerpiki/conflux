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
#   JOBS         parallel jobs for cmake --build (default: nproc)
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

if [[ -z "${SOURCE_DIR:-}" ]]; then
    SOURCE_DIR="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
fi
SOURCE_DIR="$(realpath "$SOURCE_DIR")"

JOBS="${JOBS:-$(nproc)}"

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

# ── run ──────────────────────────────────────────────────────────────────────

declare -A RESULTS=()

for preset in "${MATRIX[@]}"; do
    in_only "$preset" || continue

    printf '\n━━━ %s ━━━\n' "$preset"

    build_dir="/tmp/$(basename "$SOURCE_DIR")/$preset"
    status=PASS

    if ! cmake --preset "$preset" -S "$SOURCE_DIR" 2>&1; then
        status=CONFIGURE_FAIL
    fi

    if [[ "$status" == PASS ]]; then
        if ! cmake --build --preset "$preset" -j "$JOBS" 2>&1; then
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
