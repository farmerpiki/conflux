#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_root="${TMPDIR:-/tmp}/conflux-release-artifacts"
build_dir="${tmp_root}/build"
stage_dir="${tmp_root}/stage"
tarball=""
build_dir_set=0
preset="release-header-artifacts"
release_sku="release-json"

usage() {
    cat <<'USAGE'
usage: scripts/stage-release-artifacts.sh [--build-dir DIR] [--stage-dir DIR] [--tarball FILE] [--no-tarball] [--source-root DIR] [--release-sku NAME]

Stages the modules-first preview release artifact shape:
  source/      tracked module sources and release docs
  install/     installed package config plus generated header release artifacts
  artifacts/   module-header bridge manifest and staging manifest
USAGE
}

while (($#)); do
    case "$1" in
        --build-dir)
            build_dir="$2"
            build_dir_set=1
            shift 2
            ;;
        --stage-dir)
            stage_dir="$2"
            shift 2
            ;;
        --tarball)
            tarball="$2"
            shift 2
            ;;
        --no-tarball)
            tarball=""
            shift
            ;;
        --source-root)
            root="$(cd "$2" && pwd)"
            shift 2
            ;;
        --release-sku)
            release_sku="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'stage-release-artifacts: unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

prepare_dir() {
    local dir="$1"
    local marker="$dir/.conflux-stage-release-artifacts"
    if [[ -d "$dir" && ! -f "$marker" ]]; then
        printf 'stage-release-artifacts: refusing to clean unmarked directory: %s\n' "$dir" >&2
        exit 1
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    touch "$marker"
}

if [[ "$build_dir_set" -eq 0 && "$root" == "$(pwd)" ]]; then
    build_dir="$(python3 "$root/scripts/cmake-preset-build-dir.py" "$root" "$preset")"
fi

build_dir="$(mkdir -p "$(dirname "$build_dir")" && cd "$(dirname "$build_dir")" && pwd)/$(basename "$build_dir")"
stage_dir="$(mkdir -p "$(dirname "$stage_dir")" && cd "$(dirname "$stage_dir")" && pwd)/$(basename "$stage_dir")"
feature_set="$(python3 "$root/scripts/release-sku-field.py" "$root" "$release_sku" feature_set)"
sku_components="$(python3 "$root/scripts/release-sku-field.py" "$root" "$release_sku" components)"
mapfile -t sku_examples < <(python3 "$root/scripts/release-sku-field.py" "$root" "$release_sku" examples)
mapfile -t sku_docs < <(python3 "$root/scripts/release-sku-field.py" "$root" "$release_sku" docs)

prepare_dir "$build_dir"
prepare_dir "$stage_dir"

if [[ "$build_dir_set" -eq 0 && "$root" == "$(pwd)" ]]; then
    cmake --preset "$preset"
    cmake --build --preset "$preset"
else
    cmake -S "$root" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCONFLUX_FEATURE_SET="$feature_set" \
        -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
        -DCONFLUX_BUILD_TESTS=OFF \
        -DCONFLUX_BUILD_EXAMPLES=OFF \
        -DCONFLUX_BUILD_BENCHMARKS=OFF \
        -DCONFLUX_POSTGRES_PROVIDER=OFF
    cmake --build "$build_dir"
fi
cmake --install "$build_dir" --prefix "$stage_dir/install"

mkdir -p "$stage_dir/source" "$stage_dir/artifacts"
tar -C "$root" -cf - \
    CMakeLists.txt \
    CMakePresets.json \
    CHANGELOG.md \
    LICENSE \
    NOTICE \
    README.md \
    RELEASE_POLICY.md \
    SECURITY.md \
    SUPPORT.md \
    cmake \
    docs \
    scripts/generate-public-header-include-smoke.py \
    scripts/release-sku-field.py \
    scripts/module_header_bridge.py \
    src \
    tests \
    | tar -C "$stage_dir/source" -xf -

cp "$build_dir/generated/bridge/module_header_bridge_manifest.json" \
    "$stage_dir/artifacts/module-header-bridge-manifest.json"
cp "$root/docs/releases/evidence-template.md" "$stage_dir/evidence-template.md"
cp -a "$stage_dir/install/include" "$stage_dir/source/include"
mkdir -p "$stage_dir/source/examples/$release_sku"
for example in "${sku_examples[@]}"; do
    cp -a "$root/$example" "$stage_dir/source/examples/$release_sku/"
done
mkdir -p "$stage_dir/source/docs/$release_sku"
for doc in "${sku_docs[@]}"; do
    cp -a "$root/$doc" "$stage_dir/source/docs/$release_sku/"
done

package_config="$(find "$stage_dir/install" -path '*/cmake/conflux/conflux-config.cmake' -print -quit)"
if [[ -z "$package_config" ]]; then
    printf 'stage-release-artifacts: missing installed conflux-config.cmake\n' >&2
    exit 1
fi

{
    printf 'artifact_contract=modules-first-preview\n'
    printf 'source_root=%s\n' "$root"
    printf 'build_dir=%s\n' "$build_dir"
    printf 'stage_dir=%s\n' "$stage_dir"
    printf 'primary_interface=MODULE_INTERFACE\n'
    printf 'release_sku=%s\n' "$release_sku"
    printf 'feature_set=%s\n' "$feature_set"
    printf 'package_components=%s\n' "$sku_components"
    printf 'generated_header_artifact=install/include/conflux\n'
    printf 'source_generated_header_artifact=source/include/conflux\n'
    printf 'selected_examples=source/examples/%s\n' "$release_sku"
    printf 'selected_docs=source/docs/%s\n' "$release_sku"
    printf 'bridge_manifest=artifacts/module-header-bridge-manifest.json\n'
    printf 'installed_package_config=%s\n' "${package_config#"$stage_dir/"}"
} > "$stage_dir/release-artifact-manifest.txt"

python3 "$root/scripts/check-release-artifact.py" "$stage_dir"

if [[ -n "$tarball" ]]; then
    mkdir -p "$(dirname "$tarball")"
    tar -C "$(dirname "$stage_dir")" -czf "$tarball" "$(basename "$stage_dir")"
    printf 'stage-release-artifacts: wrote %s\n' "$tarball"
fi

printf 'stage-release-artifacts: staged %s\n' "$stage_dir"
