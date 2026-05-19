#!/usr/bin/env bash
set -euo pipefail

if (($# < 3)); then
	printf 'usage: %s <build-dir> <target> <expected-substring> [expected-substring...]\n' "$0" >&2
	exit 2
fi

build_dir=$1
target=$2
shift 2

log=$(mktemp)
trap 'rm -f "$log"' EXIT

set +e
cmake --build "$build_dir" --target "$target" >"$log" 2>&1
status=$?
set -e

if ((status == 0)); then
	printf 'compile-fail target unexpectedly built: %s\n' "$target" >&2
	cat "$log" >&2
	exit 1
fi

for expected in "$@"; do
	if ! grep -F -- "$expected" "$log" >/dev/null; then
		printf 'compile-fail target %s did not contain expected diagnostic substring: %s\n' "$target" "$expected" >&2
		cat "$log" >&2
		exit 1
	fi
done
