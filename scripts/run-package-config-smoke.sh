#!/usr/bin/env bash
set -euo pipefail

source_root=""
prefix=""
build_dir=""
components="core"
jobs="${CONFLUX_BUILD_JOBS:-}"

usage() {
    cat >&2 <<'USAGE'
usage: run-package-config-smoke.sh --source <source-root> --prefix <install-prefix> [--build-dir <dir>] [--components <list>] [--jobs <n>]

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
        --jobs)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            jobs="$2"
            shift 2
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
[[ -d "$prefix" ]] || { printf 'run-package-config-smoke: prefix does not exist: %s\n' "$prefix" >&2; exit 1; }
[[ -d "$source_root/cmake/package-smoke" ]] || { printf 'run-package-config-smoke: missing package-smoke project under %s\n' "$source_root" >&2; exit 1; }
[[ -n "$build_dir" ]] || build_dir="${TMPDIR:-/tmp}/conflux-package-smoke"

if [[ -z "$jobs" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        jobs="$(nproc)"
    else
        jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')"
    fi
fi

rm -rf "$build_dir"
cmake -S "$source_root/cmake/package-smoke" \
    -B "$build_dir" \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCONFLUX_PACKAGE_SMOKE_COMPONENTS="$components"
cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure

printf 'run-package-config-smoke: ok (%s)\n' "$components"
