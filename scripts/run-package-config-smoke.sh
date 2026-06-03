#!/usr/bin/env bash
set -euo pipefail

source_root=""
prefix=""
build_dir=""
components="core"
interface_mode=""
api_surface=""
enable_db="OFF"
forbidden_components=""
forbidden_external_deps=""
mixed_module_header="OFF"
public_module_imports="OFF"
enable_import_std="OFF"

usage() {
    cat >&2 <<'USAGE'
usage: run-package-config-smoke.sh --source <source-root> --prefix <install-prefix> [--build-dir <dir>] [--components <list>] [--interface-mode <MODULE_INTERFACE|HEADER_INTERFACE>] [--api-surface <curated|extended|complete>] [--enable-db] [--forbid-components <list>] [--forbid-external-deps <list>] [--mixed-module-header] [--public-module-imports] [--enable-import-std]

Configures and builds the package smoke project against an installed conflux
prefix. The component list is a semicolon-separated CMake list, for example:
core;json.
USAGE
}

while (($#)); do
    case "$1" in
        --source)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            source_root="$2"
            shift 2
            ;;
        --prefix)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            prefix="$2"
            shift 2
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --components)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            components="$2"
            shift 2
            ;;
        --interface-mode)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            interface_mode="$2"
            shift 2
            ;;
        --api-surface)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            api_surface="$2"
            shift 2
            ;;
        --enable-db)
            enable_db="ON"
            shift
            ;;
        --forbid-components)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            forbidden_components="$2"
            shift 2
            ;;
        --forbid-external-deps)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            forbidden_external_deps="$2"
            shift 2
            ;;
        --mixed-module-header)
            mixed_module_header="ON"
            shift
            ;;
        --public-module-imports)
            public_module_imports="ON"
            shift
            ;;
        --enable-import-std)
            enable_import_std="ON"
            shift
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

[[ -n "$source_root" ]] || source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -n "$prefix" ]] || { usage; exit 2; }
if [[ -n "$interface_mode" && "$interface_mode" != "MODULE_INTERFACE" && "$interface_mode" != "HEADER_INTERFACE" ]]; then
    printf 'run-package-config-smoke: invalid interface mode: %s\n' "$interface_mode" >&2
    exit 2
fi
if [[ -n "$api_surface" && "$api_surface" != "curated" && "$api_surface" != "extended" && "$api_surface" != "complete" ]]; then
    printf 'run-package-config-smoke: invalid API surface: %s\n' "$api_surface" >&2
    exit 2
fi
[[ -d "$prefix" ]] || { printf 'run-package-config-smoke: prefix does not exist: %s\n' "$prefix" >&2; exit 1; }
[[ -d "$source_root/cmake/package-smoke" ]] || { printf 'run-package-config-smoke: missing package-smoke project under %s\n' "$source_root" >&2; exit 1; }
[[ -n "$build_dir" ]] || build_dir="${TMPDIR:-/tmp}/conflux-package-smoke"
cleanup_build_dir() {
    if [[ "${KEEP_BUILD:-0}" != "1" && -n "$build_dir" && "$build_dir" == /tmp/* && -d "$build_dir" ]]; then
        rm -rf "$build_dir"
    fi
}
trap cleanup_build_dir EXIT

external_deps_except() {
    python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root" --exclude "$@"
}
forbidden_components_for() {
    python3 "$source_root/scripts/package-smoke-forbidden-components.py" "$1"
}
forbid_all_external_deps="$(python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root")"
forbid_external_deps_without_json_hash="$(external_deps_except XXHASH)"
forbid_template_external_deps="$(external_deps_except XXHASH OPENSSL ZLIB LIBDEFLATE ZLIB_NG LIBISAL BROTLI ZSTD)"
forbid_dns_external_deps="$(external_deps_except LIBURING XXHASH)"
forbid_pg_external_deps="$(external_deps_except LIBURING XXHASH LIBPQ)"

if [[ "$components" != *";"* ]]; then
    case "$components" in
        core)
            forbidden_components="$(forbidden_components_for core)${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="${forbid_all_external_deps}${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        json)
            forbidden_components="$(forbidden_components_for json)${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="${forbid_external_deps_without_json_hash}${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        template)
            forbidden_components="$(forbidden_components_for template)${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="${forbid_template_external_deps}${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        dns)
            forbidden_components="$(forbidden_components_for dns)${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="${forbid_dns_external_deps}${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        pg)
            forbidden_components="$(forbidden_components_for pg)${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="${forbid_pg_external_deps}${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        http)
            forbidden_components="$(forbidden_components_for http)${forbidden_components:+;$forbidden_components}"
            ;;
    esac
fi

normalize_cmake_list() {
    local input="$1"
    local output=""
    local item=""
    local -a items=()
    local -A seen=()
    IFS=';' read -r -a items <<< "$input"
    for item in "${items[@]}"; do
        [[ -n "$item" ]] || continue
        [[ -z "${seen[$item]+x}" ]] || continue
        seen[$item]=1
        output="${output:+$output;}$item"
    done
    printf '%s\n' "$output"
}

components="$(normalize_cmake_list "$components")"
forbidden_components="$(normalize_cmake_list "$forbidden_components")"
forbidden_external_deps="$(normalize_cmake_list "$forbidden_external_deps")"
if [[ -z "$components" ]]; then
    printf 'run-package-config-smoke: --components must not be empty\n' >&2
    exit 2
fi
IFS=';' read -r -a requested_components <<< "$components"
for component in "${requested_components[@]}"; do
    if [[ "$component" == _* || "$component" == headers || "$component" == header_impl || "$component" == header_impl_* ]]; then
        printf 'run-package-config-smoke: --components must request public components, not support component: %s\n' "$component" >&2
        exit 2
    fi
done

cmake_configure=(
    cmake -S "$source_root/cmake/package-smoke"
    -B "$build_dir"
    -G Ninja
    -DCMAKE_PREFIX_PATH="$prefix"
    -DCONFLUX_PACKAGE_SMOKE_COMPONENTS="$components"
    -DCONFLUX_PACKAGE_SMOKE_API_SURFACE="$api_surface"
    -DCONFLUX_PACKAGE_SMOKE_ENABLE_DB="$enable_db"
    -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS="$forbidden_components"
    -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS="$forbidden_external_deps"
    -DCONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER="$mixed_module_header"
    -DCONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS="$public_module_imports"
    -DCONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD="$enable_import_std"
)
if [[ -n "$interface_mode" ]]; then
    cmake_configure+=(-DCONFLUX_PACKAGE_SMOKE_INTERFACE_MODE="$interface_mode")
fi

rm -rf "$build_dir"
"${cmake_configure[@]}"
summary="$build_dir/conflux-package-smoke-summary.txt"
if [[ -f "$summary" ]]; then
    sed 's/^/run-package-config-smoke: /' "$summary"
fi
cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure

printf 'run-package-config-smoke: ok (%s)\n' "$components"
