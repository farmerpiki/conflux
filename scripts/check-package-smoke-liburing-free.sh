#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/smoke" \
	--components 'core;json;file_io_sync' \
	--feature-set json \
	--interface-mode HEADER_INTERFACE \
	--forbid-components 'http;http1;http2;http3;http_protocol;work;dns;template;db;pg' \
	--forbid-external-deps 'LIBURING;LIBPQ;OPENSSL;ZLIB;LIBDEFLATE;ZLIB_NG;LIBISAL;BROTLI;ZSTD;NGHTTP2;NGTCP2;NGTCP2_CRYPTO_OSSL;NGHTTP3;ARGON2' \
	--generator Ninja \
	-- -DCONFLUX_POSTGRES_PROVIDER=OFF
