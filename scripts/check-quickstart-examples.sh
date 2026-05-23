#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples/quickstart}

if grep -R -n -E 'conflux\.types|WorkPool|TaskSource|std::this_thread::sleep_for|json_response\(|http::Json\{|http::ok\(|http::codec::json|Config::(low_latency|benchmark|unsafe_max_speed)' "$root"; then
	printf 'quickstart examples use non-quickstart API surface\n' >&2
	exit 1
fi

missing_umbrella=$(grep -R -L -E '^import conflux;$' "$root"/*.cxx || true)
if [[ -n "$missing_umbrella" ]]; then
	printf '%s\n' "$missing_umbrella" >&2
	printf 'quickstart examples must import the selected curated umbrella with `import conflux;`\n' >&2
	exit 1
fi

invalid_leaf_imports=$(grep -R -n -E '^import conflux\.' "$root" \
	| grep -v -E '/json_reflect_crud\.cxx:[0-9]+:import conflux\.json\.reflect;$' \
	| grep -v -E '/postgres_json\.cxx:[0-9]+:import conflux\.pg;$' || true)
if [[ -n "$invalid_leaf_imports" ]]; then
	printf '%s\n' "$invalid_leaf_imports" >&2
	printf 'quickstart examples must use `import conflux;` plus only feature-specific leaf imports\n' >&2
	exit 1
fi

count_non_comment_lines() {
	awk 'NF && $1 !~ /^\/\// { count++ } END { print count + 0 }' "$1"
}

check_max_lines() {
	local rel=$1
	local max=$2
	local path="$root/$rel"
	if [[ ! -f "$path" ]]; then
		return
	fi
	local count
	count=$(count_non_comment_lines "$path")
	if ((count > max)); then
		printf 'quickstart example %s has %d non-comment lines, max %d\n' "$rel" "$count" "$max" >&2
		exit 1
	fi
}

check_max_lines hello.cxx 25
check_max_lines json_crud.cxx 120
check_max_lines json_reflect_crud.cxx 90
