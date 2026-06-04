#!/usr/bin/env bash
set -euo pipefail

if ! pkg-config --exists liburing; then
    printf 'check-package-smoke-runtime: skipped; liburing was not found by pkg-config\n' >&2
    exit 0
fi

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
forbid_components="$(python3 "$source_root/scripts/package-smoke-forbidden-components.py" http)"
forbid_external_deps="$(python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root" --policy http)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-runtime/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-runtime/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-runtime/smoke" \
    --components 'core;json;http;file_io_sync;work' \
    --feature-set http-minimal \
    --interface-mode HEADER_INTERFACE \
    --forbid-components "$forbid_components" \
    --forbid-external-deps "$forbid_external_deps" \
    --generator Ninja \
    -- -DCONFLUX_POSTGRES_PROVIDER=OFF
