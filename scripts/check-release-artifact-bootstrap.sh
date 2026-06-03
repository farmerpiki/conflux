#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${CONFLUX_RELEASE_BOOTSTRAP_WORK:-${TMPDIR:-/tmp}/conflux-release-artifact-bootstrap}"
release_sku="${CONFLUX_RELEASE_BOOTSTRAP_SKU:-release-json}"
stage_dir="$work_root/stage"
bootstrap_source="$work_root/source"
header_build="$work_root/header-build"
module_build="$work_root/module-build"
bootstrap_prefix="$work_root/prefix"
package_smoke_build="$work_root/package-smoke"
feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" feature_set)"
sku_components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" components)"

"$source_root/scripts/stage-release-artifacts.sh" \
    --stage-dir "$stage_dir" \
    --release-sku "$release_sku" \
    --no-tarball

rm -rf "$bootstrap_source" "$header_build" "$module_build" "$bootstrap_prefix" "$package_smoke_build"
mkdir -p "$work_root"
cp -a "$stage_dir/source" "$bootstrap_source"

if [[ -e "$bootstrap_source/.git" ]]; then
    printf 'check-release-artifact-bootstrap: staged source must not contain .git\n' >&2
    exit 1
fi

cmake -S "$bootstrap_source" -B "$header_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCONFLUX_FEATURE_SET="$feature_set" \
    -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    -DCONFLUX_POSTGRES_PROVIDER=OFF
cmake --build "$header_build"
cmake --install "$header_build" --prefix "$bootstrap_prefix"

package_smoke_configure=(
    cmake -S "$bootstrap_source/cmake/package-smoke" -B "$package_smoke_build" -G Ninja
    -DCMAKE_PREFIX_PATH="$bootstrap_prefix"
    -DCONFLUX_PACKAGE_SMOKE_COMPONENTS="$sku_components"
    -DCONFLUX_PACKAGE_SMOKE_INTERFACE_MODE=HEADER_INTERFACE
)
if [[ "$release_sku" == "release-json" ]]; then
    forbid_components="$(python3 "$source_root/scripts/package-smoke-forbidden-components.py" json)"
    forbid_external_deps_without_json_hash="$(
        python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root" --policy json
    )"
    package_smoke_configure+=(
        -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS="$forbid_components"
        -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS="$forbid_external_deps_without_json_hash"
    )
fi
"${package_smoke_configure[@]}"
cmake --build "$package_smoke_build"
ctest --test-dir "$package_smoke_build" --output-on-failure

cmake -S "$bootstrap_source" -B "$module_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCONFLUX_FEATURE_SET="$feature_set" \
    -DCONFLUX_INTERFACE_MODE=MODULE_INTERFACE \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    -DCONFLUX_POSTGRES_PROVIDER=OFF
cmake --build "$module_build"

for build_dir in "$header_build" "$module_build" "$package_smoke_build"; do
    if [[ -d "$build_dir/_deps" ]]; then
        printf 'check-release-artifact-bootstrap: unexpected FetchContent _deps directory in %s\n' "$build_dir" >&2
        exit 1
    fi
done

printf 'check-release-artifact-bootstrap: ok (%s)\n' "$bootstrap_source"
