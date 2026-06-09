#!/usr/bin/env bash
# Sanitizer CI matrix: build + test correctness presets.
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
#   ./scripts/run-sanitizer-matrix.sh [--full-release-gate] [--only PRESET,...] [-- <ctest-args>]
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
FULL_RELEASE_GATE_MATRIX=(
    asan-clang-libcxx-full
    asan-gcc16-stdcxx-full
    tsan-clang-libcxx-full
    tsan-gcc16-stdcxx-full
)

ONLY=()
CTEST_EXTRA=()
FULL_RELEASE_GATE=0
LOG_ROOT=""
BUILD_ROOT=""

while (($# > 0)); do
    case "$1" in
        --full-release-gate)
            FULL_RELEASE_GATE=1
            shift
            ;;
        --log-root)
            shift
            if (($# == 0)) || [[ -z "$1" ]]; then
                printf '%s\n' '--log-root requires a non-empty directory.' >&2
                exit 2
            fi
            LOG_ROOT="$1"
            shift
            ;;
        --build-root)
            shift
            if (($# == 0)) || [[ -z "$1" ]]; then
                printf '%s\n' '--build-root requires a non-empty directory.' >&2
                exit 2
            fi
            BUILD_ROOT="$1"
            shift
            ;;
        --only)
            shift
            if (($# == 0)) || [[ -z "$1" ]]; then
                printf '%s\n' '--only requires a non-empty comma-separated preset list.' >&2
                exit 2
            fi
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
            printf 'usage: %s [--full-release-gate] [--log-root DIR] [--build-root DIR] [--only PRESET,...] [-- <ctest-args>]\n' "$0" >&2
            exit 2
            ;;
    esac
done

if ((FULL_RELEASE_GATE == 1)); then
    MATRIX=("${FULL_RELEASE_GATE_MATRIX[@]}")
fi

script_repo_root() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$script_dir/.." && pwd
}

if [[ -z "${SOURCE_DIR:-}" ]]; then
    SOURCE_DIR="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null || script_repo_root)"
fi
SOURCE_DIR="$(realpath "$SOURCE_DIR")"
if [[ -z "$LOG_ROOT" ]]; then
    LOG_ROOT="${TMPDIR:-/tmp}/$(basename "$SOURCE_DIR")/sanitizer-matrix-logs"
fi
if [[ -z "$BUILD_ROOT" ]]; then
    BUILD_ROOT="${TMPDIR:-/tmp}/$(basename "$SOURCE_DIR")/sanitizer-matrix-builds"
fi
mkdir -p "$LOG_ROOT"
mkdir -p "$BUILD_ROOT"

if ((FULL_RELEASE_GATE == 0)); then
    for preset in "${MATRIX[@]}"; do
        python3 "$SOURCE_DIR/scripts/cmake-preset-build-dir.py" "$SOURCE_DIR" "$preset" >/dev/null
    done
fi
for preset in "${ONLY[@]+"${ONLY[@]}"}"; do
    if [[ " ${MATRIX[*]} " != *" $preset "* ]]; then
        printf 'unknown sanitizer matrix preset: %s\n' "$preset" >&2
        exit 2
    fi
done

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
    local preset="$1" build_dir="$2" surface="${3:-}"
    local asan ubsan tsan benches examples tests lto build_type allow_sanitized_benches
    asan="$(cache_value "$build_dir" CONFLUX_ENABLE_ASAN)"
    ubsan="$(cache_value "$build_dir" CONFLUX_ENABLE_UBSAN)"
    tsan="$(cache_value "$build_dir" CONFLUX_ENABLE_TSAN)"
    benches="$(cache_value "$build_dir" CONFLUX_BUILD_BENCHMARKS)"
    examples="$(cache_value "$build_dir" CONFLUX_BUILD_EXAMPLES)"
    tests="$(cache_value "$build_dir" CONFLUX_BUILD_TESTS)"
    lto="$(cache_value "$build_dir" CONFLUX_ENABLE_LTO)"
    build_type="$(cache_value "$build_dir" CMAKE_BUILD_TYPE)"
    allow_sanitized_benches="$(cache_value "$build_dir" CONFLUX_ALLOW_SANITIZED_BENCHMARKS)"

    if [[ "$surface" != "examples" && "$surface" != "benchmarks" && "$tests" != ON ]]; then
        printf '%s does not build tests; sanitizer/correctness presets must run tests.\n' "$preset" >&2
        return 1
    fi
    if [[ "$lto" != OFF ]]; then
        printf '%s enables LTO; sanitizer/correctness presets keep LTO disabled for clearer diagnostics.\n' "$preset" >&2
        return 1
    fi
    case "$preset" in
        *-full)
            case "$surface" in
                examples)
                    [[ "$examples" == ON && "$benches" == OFF && "$tests" == ON ]] || {
                        printf '%s examples surface expected examples/tests only, got tests=%s examples=%s benchmarks=%s.\n' \
                            "$preset" "$tests" "$examples" "$benches" >&2
                        return 1
                    }
                    ;;
                benchmarks)
                    [[ "$examples" == OFF && "$benches" == ON && "$tests" == OFF && "$allow_sanitized_benches" == ON ]] || {
                        printf '%s benchmark surface expected benchmarks with sanitizer waiver only, got tests=%s examples=%s benchmarks=%s allow=%s.\n' \
                            "$preset" "$tests" "$examples" "$benches" "$allow_sanitized_benches" >&2
                        return 1
                    }
                    ;;
                tests)
                    [[ "$examples" == OFF && "$benches" == OFF && "$tests" == ON ]] || {
                        printf '%s test surface expected tests only, got tests=%s examples=%s benchmarks=%s.\n' \
                            "$preset" "$tests" "$examples" "$benches" >&2
                        return 1
                    }
                    ;;
                *)
                    if [[ "$benches" != ON || "$examples" != ON || "$allow_sanitized_benches" != ON ]]; then
                        printf '%s expected tests/examples/benchmarks with sanitized benchmark waiver, got tests=%s examples=%s benchmarks=%s allow=%s.\n' \
                            "$preset" "$tests" "$examples" "$benches" "$allow_sanitized_benches" >&2
                        return 1
                    fi
                    ;;
            esac
            ;;
        *)
            if [[ "$benches" != OFF ]]; then
                printf '%s builds benchmarks; sanitizer/correctness presets must not build perf artifacts.\n' "$preset" >&2
                return 1
            fi
            ;;
    esac

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
        asan-*-full)
            [[ "$build_type" == Debug && "$asan" == ON && "$ubsan" == ON && "$tsan" == OFF ]] || {
                printf '%s expected Debug + ASan+UBSan only, got type=%s asan=%s ubsan=%s tsan=%s.\n' \
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

run_logged() {
    local log_file="$1"
    shift
    if "$@" >"$log_file" 2>&1; then
        return 0
    fi
    grep -Ei '(^FAILED:|fatal error:|error:|warning:|No space left on device|SUMMARY:|ThreadSanitizer|AddressSanitizer|UndefinedBehaviorSanitizer)' "$log_file" | tail -80 >&2 || true
    return 1
}

reject_log_warnings() {
    local preset="$1" phase="$2" log_file="$3"
    if grep -En '(^CMake Warning|(^|[[:space:]])warning:|(^|[[:space:]])WARNING:)' "$log_file" >/dev/null; then
        printf '%s %s emitted warnings; see %s\n' "$preset" "$phase" "$log_file" >&2
        grep -En '(^CMake Warning|(^|[[:space:]])warning:|(^|[[:space:]])WARNING:)' "$log_file" | head -40 >&2 || true
        return 1
    fi
}

full_build_dir() {
    printf '%s/%s\n' "$BUILD_ROOT" "$1"
}

configure_full_gate() {
    local preset="$1" build_dir="$2" surface="$3"
    local base_preset build_type asan ubsan tsan
    local build_tests build_examples build_benchmarks allow_sanitized_benchmarks
    local -a extra_args=()
    case "$preset" in
        asan-clang-libcxx-full)
            base_preset="debug-clang-libcxx"
            build_type="Debug"
            asan=ON
            ubsan=ON
            tsan=OFF
            ;;
        asan-gcc16-stdcxx-full)
            base_preset="debug-gcc16-stdcxx"
            build_type="Debug"
            asan=ON
            ubsan=ON
            tsan=OFF
            ;;
        tsan-clang-libcxx-full)
            base_preset="tsan-clang-libcxx"
            build_type="RelWithDebInfo"
            asan=OFF
            ubsan=OFF
            tsan=ON
            ;;
        tsan-gcc16-stdcxx-full)
            base_preset="tsan-gcc-stdcxx"
            extra_args+=(
                -DCMAKE_CXX_COMPILER=g++-16
                -DCMAKE_CXX_FLAGS=-Wno-tsan
            )
            build_type="RelWithDebInfo"
            asan=OFF
            ubsan=OFF
            tsan=ON
            ;;
        *)
            printf 'no full release gate configure rule for %s.\n' "$preset" >&2
            return 2
            ;;
    esac
    case "$surface" in
        examples)
            build_tests=ON
            build_examples=ON
            build_benchmarks=OFF
            allow_sanitized_benchmarks=OFF
            ;;
        benchmarks)
            build_tests=OFF
            build_examples=OFF
            build_benchmarks=ON
            allow_sanitized_benchmarks=ON
            ;;
        tests)
            build_tests=ON
            build_examples=OFF
            build_benchmarks=OFF
            allow_sanitized_benchmarks=OFF
            ;;
        *)
            printf 'unknown full release gate surface: %s\n' "$surface" >&2
            return 2
            ;;
    esac

    rm -rf "$build_dir"
    cmake -Wno-dev --preset "$base_preset" -S "$SOURCE_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DCONFLUX_ENABLE_ASAN="$asan" \
        -DCONFLUX_ENABLE_UBSAN="$ubsan" \
        -DCONFLUX_ENABLE_TSAN="$tsan" \
        -DCONFLUX_ENABLE_LTO=OFF \
        -DCONFLUX_BUILD_TESTS="$build_tests" \
        -DCONFLUX_BUILD_EXAMPLES="$build_examples" \
        -DCONFLUX_BUILD_BENCHMARKS="$build_benchmarks" \
        -DCONFLUX_ALLOW_SANITIZED_BENCHMARKS="$allow_sanitized_benchmarks" \
        -DCONFLUX_PG_TEST_CONNINFO="$PG_TEST_CONNINFO" \
        "${extra_args[@]}"
}

run_full_gate_profile() {
    local preset="$1" profile_root="$2" preset_log_root="$3"
    local surface build_dir status surface_log_root
    for surface in examples benchmarks tests; do
        build_dir="$profile_root/$surface"
        surface_log_root="$preset_log_root/$surface"
        rm -rf "$surface_log_root"
        mkdir -p "$surface_log_root"
        status=PASS

        if ! run_logged "$surface_log_root/configure.log" configure_full_gate "$preset" "$build_dir" "$surface"; then
            printf '%s %s configure failed.\n' "$preset" "$surface" >&2
            echo CONFIGURE_FAIL
            return 0
        elif ! reject_log_warnings "$preset/$surface" configure "$surface_log_root/configure.log"; then
            echo CONFIGURE_WARNING
            return 0
        fi

        if ! assert_sanitizer_cache "$preset" "$build_dir" "$surface"; then
            echo PRESET_SHAPE_FAIL
            return 0
        fi

        case "$surface" in
            examples)
                if ! run_logged "$surface_log_root/build.log" cmake --build "$build_dir" --target conflux_examples; then
                    status=BUILD_FAIL
                elif ! reject_log_warnings "$preset/$surface" build "$surface_log_root/build.log"; then
                    status=BUILD_WARNING
                fi
                ;;
            benchmarks)
                if ! run_logged "$surface_log_root/build.log" cmake --build "$build_dir" --target conflux_record_benches; then
                    status=BUILD_FAIL
                elif ! reject_log_warnings "$preset/$surface" build "$surface_log_root/build.log"; then
                    status=BUILD_WARNING
                fi
                ;;
            tests)
                if ! run_logged "$surface_log_root/build.log" cmake --build "$build_dir"; then
                    status=BUILD_FAIL
                elif ! reject_log_warnings "$preset/$surface" build "$surface_log_root/build.log"; then
                    status=BUILD_WARNING
                elif ! run_logged "$surface_log_root/ctest.log" env \
                    PG_TEST_CONNINFO="$PG_TEST_CONNINFO" \
                    PG_CONNINFO="$PG_CONNINFO" \
                    ctest --test-dir "$build_dir" \
                          --output-on-failure \
                          "${CTEST_EXTRA[@]+"${CTEST_EXTRA[@]}"}"; then
                    status=TEST_FAIL
                elif ! reject_log_warnings "$preset/$surface" ctest "$surface_log_root/ctest.log"; then
                    status=TEST_WARNING
                fi
                ;;
        esac

        if [[ "$status" != PASS ]]; then
            echo "$status"
            return 0
        fi
        rm -rf "$build_dir"
    done
    echo PASS
}

# ── run ──────────────────────────────────────────────────────────────────────

declare -A RESULTS=()
selected=0

for preset in "${MATRIX[@]}"; do
    in_only "$preset" || continue
    selected=$((selected + 1))

    printf '\n━━━ %s ━━━\n' "$preset"

    if ((FULL_RELEASE_GATE == 1)); then
        build_dir="$(full_build_dir "$preset")"
    else
        build_dir="$(python3 "$SOURCE_DIR/scripts/cmake-preset-build-dir.py" "$SOURCE_DIR" "$preset")"
    fi
    status=PASS

    preset_log_root="$LOG_ROOT/$preset"
    rm -rf "$preset_log_root"
    mkdir -p "$preset_log_root"

    if ((FULL_RELEASE_GATE == 1)); then
        status="$(run_full_gate_profile "$preset" "$build_dir" "$preset_log_root")"
    elif ! run_logged "$preset_log_root/configure.log" cmake -Wno-dev --preset "$preset" -S "$SOURCE_DIR"; then
        status=CONFIGURE_FAIL
    elif ! reject_log_warnings "$preset" configure "$preset_log_root/configure.log"; then
        status=CONFIGURE_WARNING
    fi

    if [[ "$status" == PASS && "$FULL_RELEASE_GATE" == 0 ]]; then
        if ! assert_sanitizer_cache "$preset" "$build_dir"; then
            status=PRESET_SHAPE_FAIL
        fi
    fi

    if [[ "$status" == PASS && "$FULL_RELEASE_GATE" == 0 ]]; then
        if ! run_logged "$preset_log_root/build.log" cmake --build --preset "$preset"; then
            status=BUILD_FAIL
        elif ! reject_log_warnings "$preset" build "$preset_log_root/build.log"; then
            status=BUILD_WARNING
        fi
    fi

    if [[ "$status" == PASS && "$FULL_RELEASE_GATE" == 0 ]]; then
        if ! run_logged "$preset_log_root/ctest.log" env \
            PG_TEST_CONNINFO="$PG_TEST_CONNINFO" \
            PG_CONNINFO="$PG_CONNINFO" \
            ctest --test-dir "$build_dir" \
                  --output-on-failure \
                  "${CTEST_EXTRA[@]+"${CTEST_EXTRA[@]}"}"; then
            status=TEST_FAIL
        elif ! reject_log_warnings "$preset" ctest "$preset_log_root/ctest.log"; then
            status=TEST_WARNING
        fi
    fi

    RESULTS[$preset]=$status
    if [[ "$status" == PASS && -d "$build_dir" ]]; then
        rm -rf "$build_dir"
    fi
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

if ((overall == 0 && FULL_RELEASE_GATE == 0)); then
    printf 'Note: debug-gcc-stdcxx runs without ASan/UBSan due to GCC 15 ICE\n'
    printf '      (cp/module.cc:10037 with sanitizers + C++26 modules).\n'
    printf '      ASan/UBSan coverage provided by debug-clang-libcxx.\n\n'
fi
printf 'logs: %s\n' "$LOG_ROOT"

exit $overall
