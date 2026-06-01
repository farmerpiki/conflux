#!/usr/bin/env bash
set -euo pipefail

root="${1:-$(pwd)}"
cd "$root"

build_root="${CONFLUX_HEADER_FIRST_CONTACT_SMOKE_BUILD_ROOT:-build/header-first-contact-smoke}"

if ! pkg-config --exists liburing; then
    printf 'header-first-contact-smoke: skipped; liburing was not found by pkg-config\n'
    exit 0
fi

printf 'header-first-contact-smoke: configure http-api\n'
cmake -S . -B "$build_root" \
    -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
    -DCONFLUX_HEADER_INTERFACE_WITH_SOURCES=ON \
    -DCONFLUX_FEATURE_SET=http-api \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF

printf 'header-first-contact-smoke: check public header hygiene\n'
cmake --build "$build_root" --target conflux_header_smoke_public_hygiene

printf 'header-first-contact-smoke: build curated API surface\n'
cmake --build "$build_root" --target conflux_header_smoke_api_surface_curated

printf 'header-first-contact-smoke: ok\n'
