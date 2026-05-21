#!/usr/bin/env bash
set -euo pipefail

source_root=""
prefix=""
build_dir=""
components="core"
interface_mode=""
enable_db="OFF"
forbidden_components=""
forbidden_external_deps=""

usage() {
    cat >&2 <<'USAGE'
usage: run-package-config-smoke.sh --source <source-root> --prefix <install-prefix> [--build-dir <dir>] [--components <list>] [--interface-mode <MODULE_INTERFACE|HEADER_INTERFACE>] [--enable-db] [--forbid-components <list>] [--forbid-external-deps <list>]

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
[[ -d "$prefix" ]] || { printf 'run-package-config-smoke: prefix does not exist: %s\n' "$prefix" >&2; exit 1; }
[[ -d "$source_root/cmake/package-smoke" ]] || { printf 'run-package-config-smoke: missing package-smoke project under %s\n' "$source_root" >&2; exit 1; }
[[ -n "$build_dir" ]] || build_dir="${TMPDIR:-/tmp}/conflux-package-smoke"

if [[ "$components" != *";"* ]]; then
    case "$components" in
        core)
            forbidden_components="http;http1;http2;http3;http_protocol;template;pg;db${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="LIBURING;LIBPQ;OPENSSL;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        json)
            forbidden_components="http;http1;http2;http3;http_protocol;template;pg;db;dns;runtime${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="LIBURING;LIBPQ;OPENSSL;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        template)
            forbidden_components="http;http1;http2;http3;http_protocol;pg;db;dns${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="LIBURING;LIBPQ;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        dns)
            forbidden_components="http;http1;http2;http3;http_protocol;template;pg;db${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="LIBPQ;OPENSSL;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        pg)
            forbidden_components="http;http1;http2;http3;http_protocol;template;db${forbidden_components:+;$forbidden_components}"
            forbidden_external_deps="OPENSSL;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2${forbidden_external_deps:+;$forbidden_external_deps}"
            ;;
        http)
            forbidden_components="template;pg;db${forbidden_components:+;$forbidden_components}"
            ;;
    esac
fi

cmake_configure=(
    cmake -S "$source_root/cmake/package-smoke"
    -B "$build_dir"
    -G Ninja
    -DCMAKE_PREFIX_PATH="$prefix"
    -DCONFLUX_PACKAGE_SMOKE_COMPONENTS="$components"
    -DCONFLUX_PACKAGE_SMOKE_ENABLE_DB="$enable_db"
    -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS="$forbidden_components"
    -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS="$forbidden_external_deps"
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
