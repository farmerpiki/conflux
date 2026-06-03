#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage_dir="${CONFLUX_RELEASE_SOURCE_ARCHIVE_STAGE:-${TMPDIR:-/tmp}/conflux-release-source-archive/stage}"

"$source_root/scripts/stage-release-artifacts.sh" \
    --stage-dir "$stage_dir" \
    --no-tarball

required_paths=(
    "$stage_dir/source/CMakeLists.txt"
    "$stage_dir/source/CMakePresets.json"
    "$stage_dir/source/CHANGELOG.md"
    "$stage_dir/source/LICENSE"
    "$stage_dir/source/README.md"
    "$stage_dir/source/RELEASE_POLICY.md"
    "$stage_dir/source/SECURITY.md"
    "$stage_dir/source/SUPPORT.md"
    "$stage_dir/source/cmake"
    "$stage_dir/source/docs"
    "$stage_dir/source/docs/release-json/json-api.md"
    "$stage_dir/source/docs/release-json/json-boundary-guide.md"
    "$stage_dir/source/docs/release-json/json-cookbook.md"
    "$stage_dir/source/docs/release-json/package-consumption.md"
    "$stage_dir/source/docs/release-json/prerelease-status.md"
    "$stage_dir/source/examples/release-json/json.cxx"
    "$stage_dir/source/examples/release-json/json_config.cxx"
    "$stage_dir/source/examples/release-json/json_diagnostics.cxx"
    "$stage_dir/source/examples/release-json/json_stream_ingest.cxx"
    "$stage_dir/source/examples/release-json/json_transform.cxx"
    "$stage_dir/source/include/conflux/features.hxx"
    "$stage_dir/source/include/conflux/json.hxx"
    "$stage_dir/source/scripts/generate-public-header-include-smoke.py"
    "$stage_dir/source/scripts/module_header_bridge.py"
    "$stage_dir/source/src"
    "$stage_dir/source/tests"
    "$stage_dir/artifacts/module-header-bridge-manifest.json"
    "$stage_dir/release-artifact-manifest.txt"
)

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
if ! grep -qx 'selected_examples=source/examples/release-json' \
        "$stage_dir/release-artifact-manifest.txt"; then
    printf 'check-release-source-archive: manifest does not record release-json selected examples\n' >&2
    exit 1
fi
if ! grep -qx 'selected_docs=source/docs/release-json' \
        "$stage_dir/release-artifact-manifest.txt"; then
    printf 'check-release-source-archive: manifest does not record release-json selected docs\n' >&2
    exit 1
fi

printf 'check-release-source-archive: ok (%s)\n' "$stage_dir/source"
