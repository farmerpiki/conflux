#!/usr/bin/env bash
# Fuzz smoke CI lane: build libFuzzer harnesses and run bounded seed-corpus tests.
#
# Usage:
#   ./scripts/run-fuzz-smoke.sh [-- <ctest-args>]
#
# Env:
#   SOURCE_DIR   path to project root (default: repo root via git)
set -euo pipefail

PRESET=fuzz-clang-stdcxx
CTEST_EXTRA=()

while (($# > 0)); do
    case "$1" in
        --)
            shift
            CTEST_EXTRA=("$@")
            break
            ;;
        *)
            printf 'unknown arg: %s\n' "$1" >&2
            printf 'usage: %s [-- <ctest-args>]\n' "$0" >&2
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

build_dir="$(python3 "$SOURCE_DIR/scripts/cmake-preset-build-dir.py" "$SOURCE_DIR" "$PRESET")"

cache_value() {
    local key="$1"
    sed -n "s/^${key}:[A-Z_]*=//p" "$build_dir/CMakeCache.txt" | tail -1
}

assert_fuzz_cache() {
    local asan ubsan tsan benches examples tests fuzz fuzz_smoke lto build_type feature_set runtime http_core http_realtime crypto tls http2 http3 db
    asan="$(cache_value CONFLUX_ENABLE_ASAN)"
    ubsan="$(cache_value CONFLUX_ENABLE_UBSAN)"
    tsan="$(cache_value CONFLUX_ENABLE_TSAN)"
    benches="$(cache_value CONFLUX_BUILD_BENCHMARKS)"
    examples="$(cache_value CONFLUX_BUILD_EXAMPLES)"
    tests="$(cache_value CONFLUX_BUILD_TESTS)"
    fuzz="$(cache_value CONFLUX_BUILD_FUZZ)"
    fuzz_smoke="$(cache_value CONFLUX_BUILD_FUZZ_SMOKE_TESTS)"
    lto="$(cache_value CONFLUX_ENABLE_LTO)"
    build_type="$(cache_value CMAKE_BUILD_TYPE)"
    feature_set="$(cache_value CONFLUX_FEATURE_SET)"
    runtime="$(cache_value CONFLUX_BUILD_RUNTIME)"
    http_core="$(cache_value CONFLUX_BUILD_HTTP_CORE)"
    http_realtime="$(cache_value CONFLUX_BUILD_HTTP_REALTIME)"
    crypto="$(cache_value CONFLUX_BUILD_CRYPTO)"
    tls="$(cache_value CONFLUX_TLS_PROVIDER)"
    http2="$(cache_value CONFLUX_HTTP2_PROVIDER)"
    http3="$(cache_value CONFLUX_HTTP3_PROVIDER)"
    db="$(cache_value CONFLUX_POSTGRES_PROVIDER)"

    [[ "$build_type" == RelWithDebInfo ]] || {
        printf '%s expected RelWithDebInfo, got %s.\n' "$PRESET" "$build_type" >&2
        return 1
    }
    [[ "$asan" == ON && "$ubsan" == ON && "$tsan" == OFF ]] || {
        printf '%s expected ASan+UBSan only, got asan=%s ubsan=%s tsan=%s.\n' \
            "$PRESET" "$asan" "$ubsan" "$tsan" >&2
        return 1
    }
    [[ "$fuzz" == ON && "$fuzz_smoke" == ON ]] || {
        printf '%s expected fuzz and fuzz smoke enabled, got fuzz=%s fuzz_smoke=%s.\n' \
            "$PRESET" "$fuzz" "$fuzz_smoke" >&2
        return 1
    }
    [[ "$feature_set" == http-api && "$runtime" == AUTO ]] || {
        printf '%s expected dependency-minimal http-api feature set with runtime AUTO, got feature_set=%s runtime=%s.\n' \
            "$PRESET" "$feature_set" "$runtime" >&2
        return 1
    }
    [[ "$http_core" != OFF && "$http_realtime" == ON && "$crypto" != OFF ]] || {
        printf '%s expected HTTP parser/realtime fuzz helpers enabled, got http_core=%s http_realtime=%s crypto=%s.\n' \
            "$PRESET" "$http_core" "$http_realtime" "$crypto" >&2
        return 1
    }
    [[ "$tls" == OFF && "$http2" == OFF && "$http3" == OFF && "$db" == OFF ]] || {
        printf '%s expected network/protocol/database integrations disabled, got tls=%s http2=%s http3=%s db=%s.\n' \
            "$PRESET" "$tls" "$http2" "$http3" "$db" >&2
        return 1
    }
    [[ "$tests" == OFF && "$benches" == OFF && "$examples" == OFF ]] || {
        printf '%s expected normal tests/benchmarks/examples disabled, got tests=%s benches=%s examples=%s.\n' \
            "$PRESET" "$tests" "$benches" "$examples" >&2
        return 1
    }
    [[ "$lto" == OFF ]] || {
        printf '%s enables LTO; fuzz smoke keeps LTO disabled for clearer diagnostics.\n' "$PRESET" >&2
        return 1
    }
}

cmake --preset "$PRESET" -S "$SOURCE_DIR"
assert_fuzz_cache
cmake --build --preset "$PRESET"
ctest_args=(
    --test-dir "$build_dir"
    --output-on-failure
    --no-tests=error
    -L fuzz-smoke
)
ctest "${ctest_args[@]}" "${CTEST_EXTRA[@]}"
