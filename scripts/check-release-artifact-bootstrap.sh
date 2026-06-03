#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${CONFLUX_RELEASE_BOOTSTRAP_WORK:-${TMPDIR:-/tmp}/conflux-release-artifact-bootstrap}"
stage_dir="$work_root/stage"
bootstrap_source="$work_root/source"
bootstrap_build="$work_root/build"
bootstrap_prefix="$work_root/prefix"
package_smoke_build="$work_root/package-smoke"

"$source_root/scripts/stage-release-artifacts.sh" \
    --stage-dir "$stage_dir" \
    --no-tarball

rm -rf "$bootstrap_source" "$bootstrap_build" "$bootstrap_prefix" "$package_smoke_build"
mkdir -p "$work_root"
cp -a "$stage_dir/source" "$bootstrap_source"

if [[ -e "$bootstrap_source/.git" ]]; then
    printf 'check-release-artifact-bootstrap: staged source must not contain .git\n' >&2
    exit 1
fi

cmake -S "$bootstrap_source" -B "$bootstrap_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCONFLUX_FEATURE_SET=release-json \
    -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
    -DCONFLUX_BUILD_TESTS=OFF \
    -DCONFLUX_BUILD_EXAMPLES=OFF \
    -DCONFLUX_BUILD_BENCHMARKS=OFF \
    -DCONFLUX_POSTGRES_PROVIDER=OFF
cmake --build "$bootstrap_build"
cmake --install "$bootstrap_build" --prefix "$bootstrap_prefix"

cmake -S "$bootstrap_source/cmake/package-smoke" -B "$package_smoke_build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$bootstrap_prefix" \
    -DCONFLUX_PACKAGE_SMOKE_COMPONENTS="core;json" \
    -DCONFLUX_PACKAGE_SMOKE_INTERFACE_MODE=HEADER_INTERFACE \
    -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS="http;http1;http2;http3;http_protocol;template;pg;db;dns;work" \
    -DCONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS="LIBURING;LIBPQ;OPENSSL;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2"
cmake --build "$package_smoke_build"
ctest --test-dir "$package_smoke_build" --output-on-failure

printf 'check-release-artifact-bootstrap: ok (%s)\n' "$bootstrap_source"
