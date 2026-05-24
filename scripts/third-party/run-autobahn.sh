#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
	printf 'usage: %s <conformance-server>\n' "$0" >&2
	exit 2
fi

server_bin="$1"
if ! command -v wstest >/dev/null 2>&1; then
	printf 'Autobahn wstest not found; skipping\n' >&2
	exit 77
fi
if ! command -v curl >/dev/null 2>&1; then
	printf 'curl not found; skipping\n' >&2
	exit 77
fi

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/conflux-autobahn.XXXXXX")"
cleanup() {
	if [[ -n "${server_pid:-}" ]]; then
		kill "$server_pid" >/dev/null 2>&1 || true
		wait "$server_pid" >/dev/null 2>&1 || true
	fi
	rm -rf "$tmp_dir"
}
trap cleanup EXIT

port="$(
	python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"

"$server_bin" --mode websocket --port "$port" >"$tmp_dir/server.log" 2>&1 &
server_pid="$!"

ready=0
for _ in $(seq 1 100); do
	if curl -s --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
		ready=1
		break
	fi
	if ! kill -0 "$server_pid" >/dev/null 2>&1; then
		printf 'conformance server exited early\n' >&2
		cat "$tmp_dir/server.log" >&2
		exit 1
	fi
	sleep 0.05
done
if [[ "$ready" != 1 ]]; then
	printf 'conformance server did not become ready\n' >&2
	cat "$tmp_dir/server.log" >&2
	exit 1
fi

cat >"$tmp_dir/fuzzingclient.json" <<JSON
{
  "outdir": "$tmp_dir/reports",
  "servers": [
    {
      "agent": "conflux",
      "url": "ws://127.0.0.1:$port/ws"
    }
  ],
  "cases": ["*"],
  "exclude-cases": [],
  "exclude-agent-cases": {}
}
JSON

wstest -m fuzzingclient -s "$tmp_dir/fuzzingclient.json"
