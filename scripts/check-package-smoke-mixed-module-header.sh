#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
generator="${CONFLUX_PACKAGE_SMOKE_GENERATOR:-Ninja}"
if [[ "$generator" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    printf 'mixed-module-header-smoke: skipped; Ninja generator unavailable\n'
    exit 0
fi
base="${TMPDIR:-/tmp}/conflux-package-smoke-mixed-module-header"
probe_dir="$base/probe"
probe_log="$base/probe.log"
rm -rf "$base"
mkdir -p "$base"
trap 'rm -rf "$base"' EXIT

if ! cmake -S "$source_root" -B "$probe_dir" -G "$generator" \
    -DCONFLUX_FEATURE_SET=core \
    -DCONFLUX_INTERFACE_MODE=MODULE_INTERFACE \
    -DCONFLUX_USE_IMPORT_STD=OFF \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    -DCONFLUX_BUILD_FUZZ=OFF >"$probe_log" 2>&1; then
    if grep -Eq 'requires CMake-discoverable C\+\+23 import std support|CXX_MODULES|module' "$probe_log"; then
        printf 'mixed-module-header-smoke: skipped; module interface unsupported by this toolchain\n'
        exit 0
    fi
    cat "$probe_log" >&2
    exit 1
fi
rm -rf "$probe_dir" "$probe_log"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$base/build" \
    --prefix "$base/prefix" \
    --smoke-build-dir "$base/smoke" \
    --components core \
    --feature-set core \
    --generator "$generator" \
    --interface-mode MODULE_INTERFACE \
    --mixed-module-header-smoke \
    -- \
    -DCONFLUX_USE_IMPORT_STD=OFF
