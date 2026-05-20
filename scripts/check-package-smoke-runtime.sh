#!/usr/bin/env bash
set -euo pipefail

if ! pkg-config --exists liburing; then
    printf 'check-package-smoke-runtime: skipped; liburing was not found by pkg-config\n' >&2
    exit 0
fi

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-runtime/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-runtime/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-runtime/smoke" \
    --components 'core;json;http;file_io_sync;runtime' \
    --feature-set http-minimal \
    --interface-mode HEADER_INTERFACE \
    --generator Ninja \
    -- -DCONFLUX_USE_MOCK_LIBURING=OFF -DCONFLUX_ENABLE_DB=OFF
