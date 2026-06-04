#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
generator="${CONFLUX_PACKAGE_SMOKE_API_SURFACE_GENERATOR:-Ninja}"
if [[ "$generator" == "Ninja" ]] && ! command -v ninja >/dev/null 2>&1; then
    printf 'package-smoke-api-surfaces: skipped; Ninja generator unavailable\n'
    exit 0
fi

default_feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-http-api feature_set)"
default_components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-http-api components)"
feature_set="${CONFLUX_PACKAGE_SMOKE_API_SURFACE_FEATURE_SET:-$default_feature_set}"
components="${CONFLUX_PACKAGE_SMOKE_API_SURFACE_COMPONENTS:-$default_components}"

if [[ "$components" == *http* || "$components" == *work* ]] && ! pkg-config --exists liburing; then
    printf 'package-smoke-api-surfaces: skipped; liburing was not found by pkg-config\n'
    exit 0
fi

base="${TMPDIR:-/tmp}/conflux-package-smoke-api-surfaces"
rm -rf "$base"
mkdir -p "$base"
trap 'rm -rf "$base"' EXIT

for interface_mode in HEADER_INTERFACE MODULE_INTERFACE; do
    for api_surface in curated extended complete; do
        CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-1}" \
        "$source_root/scripts/run-install-tree-smoke.sh" \
            --source "$source_root" \
            --build-dir "$base/${interface_mode}-${api_surface}/build" \
            --prefix "$base/${interface_mode}-${api_surface}/prefix" \
            --smoke-build-dir "$base/${interface_mode}-${api_surface}/smoke" \
            --components "$components" \
            --feature-set "$feature_set" \
            --generator "$generator" \
            --interface-mode "$interface_mode" \
            --api-surface "$api_surface" \
            -- \
            -DCONFLUX_USE_IMPORT_STD=OFF
    done
done

printf 'package-smoke-api-surfaces: ok (%s, %s)\n' "$feature_set" "$components"
