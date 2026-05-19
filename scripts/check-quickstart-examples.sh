#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples/quickstart}

if grep -R -n -E 'conflux\.types|HttpRequest|HttpResponse|WorkPool|TaskSource|std::this_thread::sleep_for|json_response\(R"\(\{|http::Json\{|Config::(low_latency|benchmark|unsafe_max_speed)' "$root"; then
	printf 'quickstart examples use non-quickstart API surface\n' >&2
	exit 1
fi

if grep -R -n -E '^import conflux\.' "$root" | grep -v -E ':import conflux\.http;$'; then
	printf 'quickstart examples must import only conflux.http from conflux modules\n' >&2
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
