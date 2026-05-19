#!/usr/bin/env bash
set -euo pipefail

source_root=""
build_dir=""
prefix=""
smoke_build_dir=""
components="core"
feature_set="core"
build_type="Release"
generator=""
extra_cmake_args=()

usage() {
    cat >&2 <<'USAGE'
usage: run-install-tree-smoke.sh [--source <source-root>] [--build-dir <dir>] [--prefix <install-prefix>] [--smoke-build-dir <dir>] [--components <list>] [--feature-set <name>] [--build-type <type>] [--generator <name>] [-- <extra cmake configure args>]

Builds and installs a fresh conflux tree, then configures, builds, links, and
runs a downstream find_package(conflux) smoke project against that install tree.
The default feature set is 'core' to keep the smoke cheap and dependency-light.
USAGE
}

while (($#)); do
    case "$1" in
        --source)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            source_root="$2"
            shift 2
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --prefix)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            prefix="$2"
            shift 2
            ;;
        --smoke-build-dir)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            smoke_build_dir="$2"
            shift 2
            ;;
        --components)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            components="$2"
            shift 2
            ;;
        --feature-set)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            feature_set="$2"
            shift 2
            ;;
        --build-type)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            build_type="$2"
            shift 2
            ;;
        --generator)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            generator="$2"
            shift 2
            ;;
        --)
            shift
            extra_cmake_args+=("$@")
            break
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

fail() {
    printf 'run-install-tree-smoke: %s\n' "$*" >&2
    exit 1
}

[[ -n "$source_root" ]] || source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -d "$source_root" ]] || fail "source does not exist: $source_root"
[[ -f "$source_root/CMakeLists.txt" ]] || fail "missing CMakeLists.txt under $source_root"
source_root="$(realpath -m "$source_root")"

base_dir="${TMPDIR:-/tmp}/conflux-install-tree-smoke"
[[ -n "$build_dir" ]] || build_dir="$base_dir/build"
[[ -n "$prefix" ]] || prefix="$base_dir/prefix"
[[ -n "$smoke_build_dir" ]] || smoke_build_dir="$base_dir/package-smoke"

cmake_configure=(
    cmake -S "$source_root" -B "$build_dir"
    -DCMAKE_BUILD_TYPE="$build_type"
    -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCONFLUX_FEATURE_SET="$feature_set"
    -DCONFLUX_BUILD_TESTS=OFF
    -DCONFLUX_BUILD_EXAMPLES=OFF
    -DCONFLUX_BUILD_BENCHMARKS=OFF
    -DCONFLUX_BUILD_FUZZ=OFF
)
if [[ -n "$generator" ]]; then
    cmake_configure+=(-G "$generator")
fi
cmake_configure+=("${extra_cmake_args[@]}")

prepare_clean_dir() {
    local path="$1"
    local label="$2"
    [[ -n "$path" ]] || fail "$label path is empty"

    local real_path
    real_path="$(realpath -m "$path")"
    [[ "$real_path" != "/" ]] || fail "refusing to clean / for $label"
    if [[ "$real_path" == "$source_root" || "$real_path" == "$source_root"/* ]]; then
        fail "refusing to clean $label inside source tree: $real_path"
    fi

    if [[ -e "$real_path" && ! -f "$real_path/.conflux-install-tree-smoke" ]]; then
        fail "refusing to clean unmarked $label directory: $real_path"
    fi

    rm -rf "$real_path"
    mkdir -p "$real_path"
    : > "$real_path/.conflux-install-tree-smoke"
}

prepare_clean_dir "$build_dir" "build"
prepare_clean_dir "$prefix" "install prefix"
prepare_clean_dir "$smoke_build_dir" "consumer build"
"${cmake_configure[@]}"
cmake --build "$build_dir" --target install
"$source_root/scripts/run-package-config-smoke.sh" \
    --source "$source_root" \
    --prefix "$prefix" \
    --build-dir "$smoke_build_dir" \
    --components "$components"

printf 'run-install-tree-smoke: ok (%s, %s)\n' "$feature_set" "$components"
