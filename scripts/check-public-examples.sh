#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples}

if grep -n -E 'json_response\(R"\(\{|Response::json\(R"\(\{|http::Json\{|Config::(low_latency|benchmark|unsafe_max_speed)|std::this_thread::sleep_for|http::defer\(' "$root"/*.cxx; then
	printf 'public examples use raw/direct JSON responses, blocking sleeps, unsafe config presets, or legacy defer spelling\n' >&2
	exit 1
fi
