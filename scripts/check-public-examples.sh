#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples}

if grep -n -E 'json_response\(R"\(\{|Response::json\(R"\(\{|Config::(low_latency|benchmark|unsafe_max_speed)|std::this_thread::sleep_for' "$root"/*.cxx; then
	printf 'public examples use raw JSON response strings, blocking sleeps, or unsafe config presets\n' >&2
	exit 1
fi
