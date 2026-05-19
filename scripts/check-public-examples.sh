#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples}

if grep -n -E 'conflux\.types|json_response\(|Response::json\(R"\(\{|http::Json\{|http::ok\(|http::codec::json|Config::(low_latency|benchmark|unsafe_max_speed)|std::this_thread::sleep_for|http::defer\(' "$root"/*.cxx; then
	printf 'public examples use legacy imports, raw/direct JSON responses, blocking sleeps, unsafe config presets, or legacy defer spelling\n' >&2
	exit 1
fi
