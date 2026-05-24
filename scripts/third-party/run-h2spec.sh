#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
	printf 'usage: %s <conformance-server>\n' "$0" >&2
	exit 2
fi

server_bin="$1"
if ! command -v h2spec >/dev/null 2>&1; then
	printf 'h2spec not found; skipping\n' >&2
	exit 77
fi
if ! command -v openssl >/dev/null 2>&1; then
	printf 'openssl not found; skipping\n' >&2
	exit 77
fi
if ! command -v curl >/dev/null 2>&1; then
	printf 'curl not found; skipping\n' >&2
	exit 77
fi

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/conflux-h2spec.XXXXXX")"
cleanup() {
	if [[ -n "${server_pid:-}" ]]; then
		kill "$server_pid" >/dev/null 2>&1 || true
		wait "$server_pid" >/dev/null 2>&1 || true
	fi
	rm -rf "$tmp_dir"
}
trap cleanup EXIT

cert="$tmp_dir/cert.pem"
key="$tmp_dir/key.pem"
openssl req -x509 -newkey rsa:2048 -keyout "$key" -out "$cert" \
	-days 1 -nodes -subj '/CN=localhost' >/dev/null 2>&1

port="$(
	python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"

"$server_bin" --mode h2 --port "$port" --cert "$cert" --key "$key" >"$tmp_dir/server.log" 2>&1 &
server_pid="$!"

ready=0
for _ in $(seq 1 100); do
	if curl -sk --http1.1 --max-time 1 "https://127.0.0.1:$port/health" >/dev/null 2>&1; then
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

h2spec -h 127.0.0.1 -p "$port" -t -k
