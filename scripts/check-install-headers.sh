#!/usr/bin/env bash
set -euo pipefail

root=${1:-.}

required=(
	"src/conflux/http.hpp"
	"src/conflux/detail/discard.hxx"
)

for header in "${required[@]}"; do
	if ! grep -F "$header" "$root/CMakeLists.txt" >/dev/null; then
		printf 'public header missing from install manifest: %s\n' "$header" >&2
		exit 1
	fi
done
