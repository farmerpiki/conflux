#!/usr/bin/env bash
set -euo pipefail

minifier=/home/claudiu/minify/minify.py
anchor=src/types_api.cxx

if [[ ! -f "$minifier" ]]; then
	printf 'minifier not found: %s\n' "$minifier" >&2
	exit 1
fi

mapfile -t files < <(git diff --cached --name-only --diff-filter=ACMR -- '*.cxx')
if ((${#files[@]} == 0)); then
	exit 0
fi

for file in "${files[@]}"; do
	if [[ ! -f "$file" ]]; then
		continue
	fi
	if [[ "$file" == "$anchor" ]]; then
		python "$minifier" -i "$file"
	else
		python "$minifier" -i "$file" -t "$anchor"
	fi
	git add -- "$file"
done
