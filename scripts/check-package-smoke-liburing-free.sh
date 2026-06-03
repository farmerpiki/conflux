#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-json feature_set)"
components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" release-json components)"
forbid_components="$(python3 "$source_root/scripts/package-smoke-forbidden-components.py" json)"
forbid_external_deps="$(python3 "$source_root/scripts/external-dependency-tokens.py" "$source_root" --exclude XXHASH)"

"$source_root/scripts/run-install-tree-smoke.sh" \
    --source "$source_root" \
    --build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/build" \
    --prefix "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/prefix" \
    --smoke-build-dir "${TMPDIR:-/tmp}/conflux-package-smoke-liburing-free/smoke" \
	--components "$components" \
	--feature-set "$feature_set" \
	--interface-mode HEADER_INTERFACE \
	--forbid-components "$forbid_components" \
	--forbid-external-deps "$forbid_external_deps" \
	--generator Ninja \
	-- -DCONFLUX_POSTGRES_PROVIDER=OFF
