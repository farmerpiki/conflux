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

run_smoke core core conflux_header_smoke_core
run_smoke json json conflux_header_smoke_json
run_smoke runtime runtime conflux_header_smoke_runtime
run_smoke http http-minimal conflux_quickstart_hello

printf 'header-component-smoke: ok\n'
