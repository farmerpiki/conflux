#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
generator="${CONFLUX_PACKAGE_SMOKE_GENERATOR:-Ninja}"
if [[ "$generator" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    printf 'mixed-module-header-smoke: skipped; Ninja generator unavailable\n'
    exit 0
fi
base="${TMPDIR:-/tmp}/conflux-package-smoke-mixed-module-header"
feature_set="${CONFLUX_PACKAGE_SMOKE_MIXED_FEATURE_SET:-http-minimal}"
components="${CONFLUX_PACKAGE_SMOKE_MIXED_COMPONENTS:-core;json;http}"

if [[ "$components" == *http* || "$components" == *work* ]] && ! pkg-config --exists liburing; then
    printf 'mixed-module-header-smoke: skipped; liburing was not found by pkg-config\n'
    exit 0
fi

compiler="${CXX:-c++}"
if [[ "$components" == *http* ]] && command -v "$compiler" >/dev/null 2>&1; then
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
        printf 'mixed-module-header-smoke: skipped; rich JSON/HTTP module smoke needs GCC 16+ or another non-ICEing module toolchain (found %s)\n' "$compiler_banner"
        exit 0
    fi
fi

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

CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-1}" \
"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$base/build" \
    --prefix "$base/prefix" \
    --smoke-build-dir "$base/smoke" \
    --components "$components" \
    --feature-set "$feature_set" \
    --generator "$generator" \
    --interface-mode MODULE_INTERFACE \
    --mixed-module-header-smoke \
    -- \
    -DCONFLUX_USE_IMPORT_STD=OFF
