#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${CONFLUX_RELEASE_OFFLINE_WORK:-${TMPDIR:-/tmp}/conflux-release-offline-bootstrap}"
release_sku="${CONFLUX_RELEASE_OFFLINE_SKU:-release-json}"
stage_dir="$work_root/stage"
bootstrap_source="$work_root/source"
bootstrap_build="$work_root/build"
feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" feature_set)"

"$source_root/scripts/stage-release-artifacts.sh" \
    --stage-dir "$stage_dir" \
    --release-sku "$release_sku" \
    --no-tarball

rm -rf "$bootstrap_source" "$bootstrap_build"
mkdir -p "$work_root"
cp -a "$stage_dir/source" "$bootstrap_source"

if [[ -e "$bootstrap_source/.git" ]]; then
    printf 'check-release-offline-bootstrap: staged source must not contain .git\n' >&2
    exit 1
fi

cmake -S "$bootstrap_source" -B "$bootstrap_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCONFLUX_FEATURE_SET="$feature_set" \
    -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    -DCONFLUX_FETCH_TEST_DEPS=OFF \
    -DCONFLUX_ENABLE_JSON_TESTSUITE=OFF \
    -DCONFLUX_JSON_HASH_PROVIDER=INTERNAL \
    -DCONFLUX_POSTGRES_PROVIDER=OFF
cmake --build "$bootstrap_build"

if [[ -d "$bootstrap_build/_deps" ]]; then
    printf 'check-release-offline-bootstrap: unexpected FetchContent _deps directory\n' >&2
    exit 1
fi

printf 'check-release-offline-bootstrap: ok (%s)\n' "$bootstrap_source"
