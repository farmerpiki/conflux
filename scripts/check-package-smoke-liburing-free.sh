#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
forbid_external_deps="$(python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root" --exclude XXHASH)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/smoke" \
	--components 'core;json;file_io_sync' \
	--feature-set json \
	--interface-mode HEADER_INTERFACE \
	--forbid-components 'http;http1;http2;http3;http_protocol;http_compression;net_tls;work;dns;template;db;pg' \
	--forbid-external-deps "$forbid_external_deps" \
	--generator Ninja \
	-- -DCONFLUX_POSTGRES_PROVIDER=OFF
