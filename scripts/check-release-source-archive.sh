#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage_dir="${CONFLUX_RELEASE_SOURCE_ARCHIVE_STAGE:-${TMPDIR:-/tmp}/conflux-release-source-archive/stage}"
release_sku="${CONFLUX_RELEASE_SOURCE_ARCHIVE_SKU:-release-json}"

"$source_root/scripts/stage-release-artifacts.sh" \
    --stage-dir "$stage_dir" \
    --release-sku "$release_sku" \
    --no-tarball

sku_components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" components)"
mapfile -t sku_examples < <(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" examples)
mapfile -t sku_docs < <(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" docs)

required_paths=(
    "$stage_dir/source/CMakeLists.txt"
    "$stage_dir/source/CMakePresets.json"
    "$stage_dir/source/CHANGELOG.md"
    "$stage_dir/source/LICENSE"
    "$stage_dir/source/NOTICE"
    "$stage_dir/source/README.md"
    "$stage_dir/source/RELEASE_POLICY.md"
    "$stage_dir/source/SECURITY.md"
    "$stage_dir/source/SUPPORT.md"
    "$stage_dir/source/cmake"
    "$stage_dir/source/docs"
    "$stage_dir/source/docs/release-skus.json"
    "$stage_dir/source/include/conflux/features.hxx"
    "$stage_dir/source/include/conflux/json.hxx"
    "$stage_dir/source/scripts/generate-public-header-include-smoke.py"
    "$stage_dir/source/scripts/package-smoke-forbidden-components.py"
    "$stage_dir/source/scripts/release-sku-field.py"
    "$stage_dir/source/scripts/module_header_bridge.py"
    "$stage_dir/source/src"
    "$stage_dir/source/tests"
    "$stage_dir/artifacts/module-header-bridge-manifest.json"
    "$stage_dir/release-artifact-manifest.txt"
)

for doc in "${sku_docs[@]}"; do
    required_paths+=("$stage_dir/source/docs/$release_sku/$(basename "$doc")")
done
for example in "${sku_examples[@]}"; do
    required_paths+=("$stage_dir/source/examples/$release_sku/$(basename "$example")")
done

for path in "${required_paths[@]}"; do
    if [[ ! -e "$path" ]]; then
        printf 'check-release-source-archive: missing %s\n' "${path#"$stage_dir/"}" >&2
        exit 1
    fi
done

if ! grep -qx 'source_generated_header_artifact=source/include/conflux' \
        "$stage_dir/release-artifact-manifest.txt"; then
    printf 'check-release-source-archive: manifest does not record source generated headers\n' >&2
    exit 1
fi
if ! grep -qx "selected_examples=source/examples/$release_sku" \
        "$stage_dir/release-artifact-manifest.txt"; then
    printf 'check-release-source-archive: manifest does not record %s selected examples\n' "$release_sku" >&2
    exit 1
fi
if ! grep -qx "selected_docs=source/docs/$release_sku" \
        "$stage_dir/release-artifact-manifest.txt"; then
    printf 'check-release-source-archive: manifest does not record %s selected docs\n' "$release_sku" >&2
    exit 1
fi
if ! grep -qx "package_components=$sku_components" \
        "$stage_dir/release-artifact-manifest.txt"; then
    printf 'check-release-source-archive: manifest does not record %s package components\n' "$release_sku" >&2
    exit 1
fi

printf 'check-release-source-archive: ok (%s)\n' "$stage_dir/source"
