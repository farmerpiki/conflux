#!/usr/bin/env bash
set -euo pipefail

if ! pkg-config --exists libpq; then
    printf 'check-package-smoke-db: skipped; libpq was not found by pkg-config\n' >&2
    exit 0
fi

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-db/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-db/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-db/smoke" \
    --components 'core;json;db' \
    --feature-set complete \
    --interface-mode HEADER_INTERFACE \
    --enable-db-smoke \
    --generator Ninja \
    -- -DCONFLUX_USE_MOCK_LIBURING=OFF -DCONFLUX_ENABLE_DB=ON
