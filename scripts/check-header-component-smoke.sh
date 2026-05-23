#!/usr/bin/env bash
set -euo pipefail

root="${1:-$(pwd)}"
cd "$root"

build_root="${CONFLUX_HEADER_COMPONENT_SMOKE_BUILD_ROOT:-build/header-component-smoke}"

run_smoke() {
    local name=$1
    local feature_set=$2
    local target=$3
    local build_dir="$build_root/$name"

    printf 'header-component-smoke: configure %s (%s)\n' "$name" "$feature_set"
    cmake -S . -B "$build_dir" \
        -DCONFLUX_USE_MOCK_LIBURING=ON \
        -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
        -DCONFLUX_HEADER_INTERFACE_WITH_SOURCES=ON \
        -DCONFLUX_FEATURE_SET="$feature_set" \
        -DCONFLUX_BUILD_TESTS=OFF \
        -DCONFLUX_BUILD_BENCHMARKS=OFF \
        -DCONFLUX_BUILD_EXAMPLES=ON

    printf 'header-component-smoke: build target %s\n' "$target"
    cmake --build "$build_dir" --target "$target" -j2
}

compiler_supports_public_include_matrix() {
    local compiler="${CXX:-c++}"
    if ! command -v "$compiler" >/dev/null 2>&1; then
        return 0
    fi

    local compiler_banner compiler_version compiler_major compiler_macros is_gcc
    compiler_banner="$($compiler --version 2>/dev/null | head -n 1 || true)"
    compiler_version="$($compiler -dumpfullversion -dumpversion 2>/dev/null || true)"
    compiler_major="${compiler_version%%.*}"
    compiler_macros="$($compiler -dM -E -x c++ /dev/null 2>/dev/null || true)"
    is_gcc=0
    if [[ "$compiler_banner" == *g++* || "$compiler_banner" == *GCC* ]]; then
        is_gcc=1
    elif [[ "$compiler_macros" == *"__GNUC__"* && "$compiler_macros" != *"__clang__"* ]]; then
        is_gcc=1
    fi

    if (( is_gcc )) && [[ "$compiler_major" =~ ^[0-9]+$ ]] && (( compiler_major < 16 )); then
        printf 'header-component-smoke: skipped public include matrix; reflection headers need GCC 16+ or another P2996-capable toolchain (found %s)\n' "$compiler_banner"
        return 1
    fi
    return 0
}

run_smoke core core conflux_header_smoke_core
run_smoke json json conflux_header_smoke_json
run_smoke runtime runtime conflux_header_smoke_runtime
run_smoke http http-minimal conflux_quickstart_hello
if compiler_supports_public_include_matrix; then
    run_smoke public-include-matrix http-minimal conflux_header_smoke_public_includes
fi
run_smoke public-hygiene http-minimal conflux_header_smoke_public_hygiene

printf 'header-component-smoke: ok\n'
