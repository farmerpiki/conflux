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
    --generator Ninja \
    -- -DCONFLUX_USE_MOCK_LIBURING=ON -DCONFLUX_ENABLE_DB=OFF
