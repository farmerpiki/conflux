#!/usr/bin/env bash
# Build and record json-proof for gcc16/clang-header/gcc15 variants.
set -euo pipefail

usage() {
    printf 'usage: %s --evidence-dir DIR [--variant VARIANT]\n' "$0" >&2
    printf '  VARIANT: gcc16 | clang-header | gcc15 | all (default: all)\n' >&2
}

evidence_dir=""
variant="all"

while (($# > 0)); do
    case "$1" in
        --evidence-dir) if (($# < 2)); then usage; exit 2; fi; evidence_dir="$2"; shift 2 ;;
        --variant)      if (($# < 2)); then usage; exit 2; fi; variant="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

if [[ -z "$evidence_dir" ]]; then usage; exit 2; fi

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_variant() {
    local v="$1"
    local build_dir="/tmp/gcc-16/json-proof-${v}"
    local log_prefix="$build_dir"
    local configure_log="${log_prefix}-configure.log"
    local build_log="${log_prefix}-build.log"
    local ctest_log="${log_prefix}-ctest.log"

    printf '[run-json-proof-variants] === variant: %s ===\n' "$v" >&2

    rm -rf "$build_dir"
    mkdir -p "/tmp/gcc-16"

    case "$v" in
        gcc16)
            cmake -Wno-dev -S "$source_root" -B "$build_dir" -G Ninja \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_COMPILER=g++-16 \
                -DCONFLUX_USE_IMPORT_STD=OFF \
                -DCONFLUX_FEATURE_SET=dev-json \
                -DCONFLUX_BUILD_TESTS=ON \
                -DCONFLUX_BUILD_EXAMPLES=OFF \
                -DCONFLUX_BUILD_BENCHMARKS=OFF \
                -DCONFLUX_FETCH_TEST_DEPS=ON \
                -DCONFLUX_TEST_CATCH2_PROVIDER=FETCH \
                -DCONFLUX_ENABLE_JSON_TESTSUITE=ON \
                >"$configure_log" 2>&1
            ;;
        clang-header)
            cmake -Wno-dev -S "$source_root" -B "$build_dir" -G Ninja \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_COMPILER=/usr/lib/llvm/21/bin/clang++ \
                -DCMAKE_CXX_FLAGS="-stdlib=libc++ -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" \
                -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++" \
                -DCMAKE_SHARED_LINKER_FLAGS="-stdlib=libc++" \
                -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
                -DCONFLUX_FEATURE_SET=dev-json \
                -DCONFLUX_BUILD_TESTS=ON \
                -DCONFLUX_BUILD_EXAMPLES=OFF \
                -DCONFLUX_BUILD_BENCHMARKS=OFF \
                -DCONFLUX_FETCH_TEST_DEPS=ON \
                -DCONFLUX_TEST_CATCH2_PROVIDER=FETCH \
                -DCONFLUX_ENABLE_JSON_TESTSUITE=ON \
                >"$configure_log" 2>&1
            ;;
        gcc15)
            cmake -Wno-dev -S "$source_root" -B "$build_dir" -G Ninja \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_COMPILER=g++ \
                -DCONFLUX_USE_IMPORT_STD=OFF \
                -DCONFLUX_FEATURE_SET=dev-json \
                -DCONFLUX_BUILD_TESTS=ON \
                -DCONFLUX_BUILD_EXAMPLES=OFF \
                -DCONFLUX_BUILD_BENCHMARKS=OFF \
                -DCONFLUX_FETCH_TEST_DEPS=ON \
                -DCONFLUX_TEST_CATCH2_PROVIDER=FETCH \
                -DCONFLUX_ENABLE_JSON_TESTSUITE=ON \
                >"$configure_log" 2>&1
            ;;
    esac

    cmake --build "$build_dir" \
        --target conflux_json_testsuite_gate conflux_json_differential_smoke \
        >"$build_log" 2>&1

    ctest --test-dir "$build_dir" \
        --output-on-failure \
        -R '^(JSONTestSuite:|json differential smoke:)' \
        >"$ctest_log" 2>&1

    "$source_root/scripts/record-json-proof-evidence-variant.sh" \
        --variant "$v" \
        --evidence-dir "$evidence_dir" \
        --build-dir "$build_dir" \
        --log-prefix "$log_prefix"

    printf '[run-json-proof-variants] variant %s: ok\n' "$v" >&2
}

case "$variant" in
    all) run_variant gcc16; run_variant clang-header; run_variant gcc15 ;;
    gcc16|clang-header|gcc15) run_variant "$variant" ;;
    *)
        printf 'run-json-proof-variants: unknown variant: %s\n' "$variant" >&2
        usage; exit 2 ;;
esac

printf 'run-json-proof-variants: all done\n' >&2
