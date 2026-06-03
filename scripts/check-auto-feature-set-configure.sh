#!/usr/bin/env bash
set -euo pipefail

source_root="${1:-$(pwd)}"
build_dir="${TMPDIR:-/tmp}/gcc-16/auto-feature-set-configure"
log_file="$build_dir/configure.log"

rm -rf "$build_dir"
mkdir -p "$build_dir"
trap 'rm -rf "$build_dir"' EXIT

cmake -S "$source_root" -B "$build_dir" -G Ninja \
    -DCONFLUX_FEATURE_SET=auto \
    -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    >"$log_file" 2>&1

grep -q "CONFLUX_FEATURE_SET=auto: quick-try mode" "$log_file" \
    || { cat "$log_file" >&2; echo "auto feature-set notice missing" >&2; exit 1; }
grep -q "conflux: auto feature set enabled components:" "$log_file" \
    || { cat "$log_file" >&2; echo "auto feature-set enabled summary missing" >&2; exit 1; }

echo "auto-feature-set configure: ok"
