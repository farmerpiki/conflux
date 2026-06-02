#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_root="${TMPDIR:-/tmp}/conflux-package-smoke-core-isolated"
strict_build_dir="$work_root/strict-build"
strict_prefix="$work_root/strict-prefix"
strict_smoke_build_dir="$work_root/strict-smoke"
json_build_dir="$work_root/json-build"
json_prefix="$work_root/json-prefix"
json_smoke_build_dir="$work_root/json-smoke"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$strict_build_dir" \
    --prefix "$strict_prefix" \
    --smoke-build-dir "$strict_smoke_build_dir" \
    --components core \
    --feature-set core \
    --interface-mode HEADER_INTERFACE \
    --generator Ninja \
    --forbid-components 'curated;extended;complete;json;http;http1;http2;http3;http_protocol;template;pg;db'

if ! pkg-config --exists libxxhash; then
    printf 'check-package-smoke-core-isolated: skipped json-feature lane; libxxhash was not found by pkg-config\n' >&2
    exit 0
fi

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "$json_build_dir" \
    --prefix "$json_prefix" \
    --smoke-build-dir "$json_smoke_build_dir" \
    --components core \
    --feature-set json \
    --interface-mode HEADER_INTERFACE \
    --generator Ninja \
    -- -DCONFLUX_POSTGRES_PROVIDER=OFF -DCONFLUX_JSON_HASH_PROVIDER=XXHASH

for forbidden_header in \
    "$strict_prefix/include/conflux/curated.hpp" \
    "$strict_prefix/include/conflux/curated.hxx" \
    "$strict_prefix/include/conflux/extended.hpp" \
    "$strict_prefix/include/conflux/extended.hxx" \
    "$strict_prefix/include/conflux/complete.hpp" \
    "$strict_prefix/include/conflux/complete.hxx" \
    "$json_prefix/include/conflux/curated.hpp" \
    "$json_prefix/include/conflux/curated.hxx" \
    "$json_prefix/include/conflux/extended.hpp" \
    "$json_prefix/include/conflux/extended.hxx" \
    "$json_prefix/include/conflux/complete.hpp" \
    "$json_prefix/include/conflux/complete.hxx" \
    "$json_prefix/include/conflux/detail/generated/net/compress_backend_zlib_like.hxx"
do
    if [[ -e "$forbidden_header" ]]; then
        printf 'check-package-smoke-core-isolated: forbidden generated detail header installed: %s\n' \
            "$forbidden_header" >&2
        exit 1
    fi
done
