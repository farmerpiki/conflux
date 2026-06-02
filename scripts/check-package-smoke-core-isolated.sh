#!/usr/bin/env bash
set -euo pipefail

if ! pkg-config --exists libxxhash; then
    printf 'check-package-smoke-core-isolated: skipped; libxxhash was not found by pkg-config\n' >&2
    exit 0
fi

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${TMPDIR:-/tmp}/conflux-package-smoke-core-isolated"
build_dir="$work_root/build"
prefix="$work_root/prefix"
smoke_build_dir="$work_root/smoke"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$build_dir" \
    --prefix "$prefix" \
    --smoke-build-dir "$smoke_build_dir" \
    --components core \
    --feature-set json \
    --interface-mode HEADER_INTERFACE \
    --generator Ninja \
    -- -DCONFLUX_POSTGRES_PROVIDER=OFF -DCONFLUX_JSON_HASH_PROVIDER=XXHASH

for forbidden_header in \
    "$prefix/include/conflux/detail/generated/net/compress_backend_zlib_like.hxx"
do
    if [[ -e "$forbidden_header" ]]; then
        printf 'check-package-smoke-core-isolated: forbidden generated detail header installed: %s\n' \
            "$forbidden_header" >&2
        exit 1
    fi
done
