#!/usr/bin/env bash
set -euo pipefail

if ! pkg-config --exists libxxhash; then
    printf 'check-package-smoke-core-isolated: skipped; libxxhash was not found by pkg-config\n' >&2
    exit 0
fi

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-core-isolated/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-core-isolated/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-core-isolated/smoke" \
    --components core \
    --feature-set json \
    --interface-mode HEADER_INTERFACE \
    --forbid-components 'http;http1;http2;http3;http_protocol;template;pg;db' \
    --forbid-external-deps 'LIBURING;LIBPQ;OPENSSL;XXHASH;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2' \
    --generator Ninja \
    -- -DCONFLUX_POSTGRES_PROVIDER=OFF -DCONFLUX_JSON_HASH_PROVIDER=XXHASH
