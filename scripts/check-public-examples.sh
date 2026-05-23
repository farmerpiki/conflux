#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples}

if grep -n -E 'conflux\.types|json_response\(|Response::json\(R"\(\{|http::Json\{|http::ok\(|http::codec::json|Config::(low_latency|benchmark|unsafe_max_speed)|std::this_thread::sleep_for|http::defer\(' "$root"/*.cxx; then
	printf 'public examples use legacy imports, raw/direct JSON responses, blocking sleeps, unsafe config presets, or legacy defer spelling\n' >&2
	exit 1
fi
# First-contact top-level HTTP examples should teach the selected curated umbrella.
for file in "$root"/hello.cxx "$root"/forms.cxx "$root"/sse.cxx "$root"/static.cxx; do
	if [[ -f "$file" ]] && ! grep -q -E '^import conflux;$' "$file"; then
		printf '%s must import selected umbrella `conflux`, not a leaf facade\n' "$file" >&2
		exit 1
	fi
done
