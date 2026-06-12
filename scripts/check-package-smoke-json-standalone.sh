#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
generator="${CONFLUX_JSON_STANDALONE_SMOKE_GENERATOR:-Ninja}"
if [[ "$generator" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    printf 'json-standalone-smoke: skipped; Ninja generator unavailable\n'
    exit 0
fi

base="${TMPDIR:-/tmp}/conflux-package-smoke-json-standalone"
feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-json feature_set)"
components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-json components)"
forbid_components="$(python3 "$source_root/scripts/package-smoke-forbidden-components.py" json)"
forbid_external_deps="$(python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root" --policy json)"

compiler="${CXX:-c++}"
if command -v "$compiler" >/dev/null 2>&1; then
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
        printf 'json-standalone-smoke: skipped; module interface needs GCC 16+ or another non-ICEing module toolchain (found %s)\n' "$compiler_banner"
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
        printf 'json-standalone-smoke: skipped; module interface unsupported by this toolchain\n'
        exit 0
    fi
    cat "$probe_log" >&2
    exit 1
fi
rm -rf "$probe_dir" "$probe_log"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$base/header-build" \
    --prefix "$base/header-prefix" \
    --smoke-build-dir "$base/header-smoke" \
    --components "$components" \
    --feature-set "$feature_set" \
    --generator "$generator" \
    --interface-mode HEADER_INTERFACE \
    --forbid-components "$forbid_components" \
    --forbid-external-deps "$forbid_external_deps" \
    -- \
    -DCONFLUX_POSTGRES_PROVIDER=OFF

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$base/module-build" \
    --prefix "$base/module-prefix" \
    --smoke-build-dir "$base/module-smoke" \
    --components "$components" \
    --feature-set "$feature_set" \
    --generator "$generator" \
    --interface-mode MODULE_INTERFACE \
    --forbid-components "$forbid_components" \
    --forbid-external-deps "$forbid_external_deps" \
    --mixed-module-header-smoke \
    --public-module-import-smoke \
    -- \
    -DCONFLUX_USE_IMPORT_STD=OFF \
    -DCONFLUX_POSTGRES_PROVIDER=OFF

printf 'json-standalone-smoke: ok (%s, %s)\n' "$feature_set" "$components"
