#!/usr/bin/env bash
set -euo pipefail

source_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
generator="${CONFLUX_PUBLIC_MODULE_IMPORT_SMOKE_GENERATOR:-Ninja}"
if [[ "$generator" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    printf 'public-module-import-smoke: skipped; Ninja generator unavailable\n'
    exit 0
fi

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
        printf 'public-module-import-smoke: skipped; exhaustive module import smoke needs GCC 16+ or another non-ICEing module toolchain (found %s)\n' "$compiler_banner"
        exit 0
    fi
fi

base="${TMPDIR:-/tmp}/conflux-public-module-import-smoke"
default_feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-http-api feature_set)"
default_components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-http-api components)"
feature_set="${CONFLUX_PUBLIC_MODULE_IMPORT_SMOKE_FEATURE_SET:-$default_feature_set}"
components="${CONFLUX_PUBLIC_MODULE_IMPORT_SMOKE_COMPONENTS:-$default_components}"

if [[ "$components" == *http* || "$components" == *work* ]] && ! pkg-config --exists liburing; then
    printf 'public-module-import-smoke: skipped; liburing was not found by pkg-config\n'
    exit 0
fi

rm -rf "$base"
mkdir -p "$base"
trap 'rm -rf "$base"' EXIT

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$base/build" \
    --prefix "$base/prefix" \
    --smoke-build-dir "$base/smoke" \
    --components "$components" \
    --feature-set "$feature_set" \
    --generator "$generator" \
    --interface-mode MODULE_INTERFACE \
    --public-module-import-smoke \
    -- \
    -DCONFLUX_USE_IMPORT_STD=OFF

printf 'public-module-import-smoke: ok (%s, %s)\n' "$feature_set" "$components"
