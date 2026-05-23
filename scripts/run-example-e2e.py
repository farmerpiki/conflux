#!/usr/bin/env python3
"""Run a built example, hit representative endpoints, then terminate it."""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class HttpResponse:
    status: str
    headers: dict[str, str]
    body: bytes


def _connect(port: int, timeout: float = 1.0) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def _wait_for_port(proc: subprocess.Popen[bytes], port: int, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"example exited before listening, code={proc.returncode}")
        try:
            with _connect(port, timeout=0.2):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"port {port} did not become ready: {last_error}")


def _read_http(sock: socket.socket) -> HttpResponse:
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
    header_end = data.find(b"\r\n\r\n")
    if header_end < 0:
        raise RuntimeError(f"incomplete HTTP response: {data!r}")
    header_bytes = bytes(data[:header_end])
    body = bytearray(data[header_end + 4 :])
    lines = header_bytes.decode("iso-8859-1").split("\r\n")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.lower()] = value.strip()
    expected = int(headers.get("content-length", len(body)))
    while len(body) < expected:
        chunk = sock.recv(expected - len(body))
        if not chunk:
            break
        body.extend(chunk)
    return HttpResponse(lines[0], headers, bytes(body))


def http_request(port: int, method: str, path: str, headers: dict[str, str] | None = None, body: bytes = b"") -> HttpResponse:
    headers = dict(headers or {})
    headers.setdefault("Host", "localhost")
    headers.setdefault("Connection", "close")
    if body:
        headers.setdefault("Content-Length", str(len(body)))
    raw = bytearray(f"{method} {path} HTTP/1.1\r\n".encode("ascii"))
    for key, value in headers.items():
        raw.extend(f"{key}: {value}\r\n".encode("ascii"))
    raw.extend(b"\r\n")
    raw.extend(body)
    with _connect(port) as sock:
        sock.sendall(raw)
        return _read_http(sock)


def assert_status(resp: HttpResponse, prefix: str) -> None:
    if not resp.status.startswith(prefix):
        raise AssertionError(f"expected {prefix!r}, got {resp.status!r}, body={resp.body[:200]!r}")


def assert_body_contains(resp: HttpResponse, needle: bytes) -> None:
    if needle not in resp.body:
        raise AssertionError(f"missing body needle {needle!r}; body={resp.body[:300]!r}")


def case_quickstart_hello() -> None:
    root = http_request(9090, "GET", "/")
    assert_status(root, "HTTP/1.1 200")
    assert_body_contains(root, b"hello from conflux")

    named = http_request(9090, "GET", "/hello/e2e")
    assert_status(named, "HTTP/1.1 200")
    assert_body_contains(named, b"Hello, e2e")


def case_quickstart_json_crud() -> None:
    empty = http_request(9110, "GET", "/todos")
    assert_status(empty, "HTTP/1.1 200")
    assert_body_contains(empty, b'"items"')

    created = http_request(
        9110,
        "POST",
        "/todos",
        {"Content-Type": "application/json"},
        b'{"title":"ship runnable examples"}',
    )
    assert_status(created, "HTTP/1.1 201")
    if "location" not in created.headers:
        raise AssertionError(f"created response missing Location: {created.headers!r}")

    fetched = http_request(9110, "GET", created.headers["location"])
    assert_status(fetched, "HTTP/1.1 200")
    assert_body_contains(fetched, b"ship runnable examples")


def case_quickstart_static_files() -> None:
    redirect = http_request(9095, "GET", "/")
    assert_status(redirect, "HTTP/1.1 30")
    if redirect.headers.get("location") != "/assets/":
        raise AssertionError(f"expected redirect to /assets/, got {redirect.headers!r}")

    asset = http_request(9095, "GET", "/assets/index.html")
    assert_status(asset, "HTTP/1.1 200")
    assert_body_contains(asset, b"conflux static files")


def case_quickstart_sse() -> None:
    resp = http_request(9091, "GET", "/events", {"Accept": "text/event-stream"})
    assert_status(resp, "HTTP/1.1 200")
    if resp.headers.get("content-type") != "text/event-stream":
        raise AssertionError(f"unexpected SSE content-type: {resp.headers!r}")
    assert_body_contains(resp, b"event: message")
    assert_body_contains(resp, b"data: event 3")


def _ws_frame(payload: bytes) -> bytes:
    mask = b"\x11\x22\x33\x44"
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return b"\x81" + bytes([0x80 | len(payload)]) + mask + masked


def _read_ws_frame(sock: socket.socket) -> bytes:
    hdr = sock.recv(2)
    if len(hdr) != 2:
        raise RuntimeError("missing websocket frame header")
    length = hdr[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", sock.recv(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", sock.recv(8))[0]
    mask = b""
    if hdr[1] & 0x80:
        mask = sock.recv(4)
    payload = bytearray()
    while len(payload) < length:
        payload.extend(sock.recv(length - len(payload)))
    if mask:
        payload = bytearray(b ^ mask[i % 4] for i, b in enumerate(payload))
    return bytes(payload)


def case_quickstart_websocket() -> None:
    key = base64.b64encode(b"conflux-e2e-key!!").decode("ascii")
    with _connect(9096, timeout=2.0) as sock:
        sock.sendall(
            (
                "GET /ws HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode("ascii")
        )
        data = bytearray()
        while b"\r\n\r\n" not in data:
            data.extend(sock.recv(4096))
        headers = bytes(data).decode("iso-8859-1")
        if not headers.startswith("HTTP/1.1 101"):
            raise AssertionError(f"websocket handshake failed: {headers!r}")
        accept_src = (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
        accept = base64.b64encode(hashlib.sha1(accept_src).digest()).decode("ascii")
        if accept not in headers:
            raise AssertionError("websocket handshake missing expected Sec-WebSocket-Accept")
        sock.sendall(_ws_frame(b"echo-me"))
        echoed = _read_ws_frame(sock)
        if echoed != b"echo-me":
            raise AssertionError(f"expected websocket echo, got {echoed!r}")


def case_production_showcase() -> None:
    health = http_request(9105, "GET", "/health")
    assert_status(health, "HTTP/1.1 200")
    assert_body_contains(health, b"ok")

    created = http_request(
        9105,
        "POST",
        "/todos",
        {"Content-Type": "application/json"},
        b'{"title":"release proof"}',
    )
    assert_status(created, "HTTP/1.1 201")
    assert_body_contains(created, b"release proof")

    unauthorized = http_request(9105, "GET", "/metrics")
    assert_status(unauthorized, "HTTP/1.1 401")

    metrics = http_request(9105, "GET", "/metrics", {"Authorization": "Bearer metrics-token"})
    assert_status(metrics, "HTTP/1.1 200")
    assert_body_contains(metrics, b"http_requests_total")


CASES: dict[str, tuple[int, Callable[[], None]]] = {
    "quickstart_hello": (9090, case_quickstart_hello),
    "quickstart_json_crud": (9110, case_quickstart_json_crud),
    "quickstart_static_files": (9095, case_quickstart_static_files),
    "quickstart_sse": (9091, case_quickstart_sse),
    "quickstart_websocket": (9096, case_quickstart_websocket),
    "production_showcase": (9105, case_production_showcase),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", required=True, choices=sorted(CASES))
    parser.add_argument("--exe", required=True)
    args = parser.parse_args()

    port, runner = CASES[args.case]
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")

    proc = subprocess.Popen(
        [args.exe],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        env=env,
    )
    try:
        _wait_for_port(proc, port)
        runner()
        return 0
    except Exception as exc:
        print(f"example e2e failed for {args.case}: {exc}", file=sys.stderr)
        return 1
    finally:
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.communicate(timeout=4)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.communicate(timeout=2)
        else:
            proc.communicate(timeout=1)
        if proc.returncode not in (0, -signal.SIGTERM, -signal.SIGKILL, None):
            stdout = (proc.stdout.read() if proc.stdout else b"")[:4000]
            stderr = (proc.stderr.read() if proc.stderr else b"")[:4000]
            if stdout:
                print(stdout.decode("utf-8", "replace"), file=sys.stderr)
            if stderr:
                print(stderr.decode("utf-8", "replace"), file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
