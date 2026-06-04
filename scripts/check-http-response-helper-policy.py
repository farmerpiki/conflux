#!/usr/bin/env python3
"""Guard the first-contact HTTP response helper policy."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def missing_markers(text: str, markers: dict[str, str]) -> list[str]:
    return [message for marker, message in markers.items() if marker not in text]


def main() -> int:
    failures: list[str] = []

    facade = read("src/net/http_facade.cxx")
    extended = read("src/net/http_facade_extended.cxx")
    docs = read("docs/http-server-api.md")
    snapshot = read("tests/http_facade_api_snapshot.cxx")
    response_tests = read("tests/http_facade_response_test.cxx")
    cost_guard = read("scripts/check-cost-lifetime-docs.py")
    compile_fail = "\n".join(
        read(path)
        for path in [
            "tests/http_facade_compile_fail_file_free.cxx",
            "tests/header_http_facade_compile_fail_file_free.cxx",
            "tests/http_facade_compile_fail_global_streamed_file.cxx",
        ]
    )

    failures.extend(
        missing_markers(
            facade,
            {
                "Response text(": "first-contact facade must keep text helper",
                "Response html(": "first-contact facade must keep html helper",
                "Response no_content()": "first-contact facade must keep no_content helper",
                "Response bad_request(": "first-contact facade must keep bad_request helper",
                "Response redirect(": "first-contact facade must keep redirect helper",
                "Response buffered_stream(": "first-contact facade must keep buffered_stream helper",
                "Response created(": "first-contact facade must keep created helper",
                "CreatedBody<T> created(": "first-contact facade must keep typed created helper",
                "Json<T> json(": "first-contact facade must keep typed JSON helper",
            },
        ),
    )
    failures.extend(
        missing_markers(
            snapshot,
            {
                "http::text(": "public facade snapshot must exercise text helper",
                "http::html(": "public facade snapshot must exercise html helper",
                "http::json(Payload": "public facade snapshot must exercise typed JSON helper",
                "http::created(Payload": "public facade snapshot must exercise typed created helper",
                "http::no_content()": "public facade snapshot must exercise no_content helper",
                "http::bad_request(": "public facade snapshot must exercise bad_request helper",
                "http::redirect(": "public facade snapshot must exercise redirect helper",
                "http::buffered_stream(": "public facade snapshot must exercise buffered stream helper",
            },
        ),
    )
    failures.extend(
        missing_markers(
            response_tests,
            {
                "http::buffered_stream": "facade response tests must cover buffered stream helper behavior",
                "http::bad_request": "facade response tests must cover bad_request helper behavior",
                "http::redirect": "facade response tests must cover redirect helper behavior",
                "http::created(FacadeAnswer": "facade response tests must cover typed created helper behavior",
                "http::cookie": "facade response tests must cover response-builder cookie behavior",
            },
        ),
    )
    failures.extend(
        missing_markers(
            docs,
            {
                "typed response helpers": "HTTP API docs must name typed response helpers in the first-contact facade",
                "`http::text/html/json(body, status)`": "HTTP API docs must document response helper status behavior",
                "`http::text(...)`": "HTTP API docs must list first-contact free response helpers",
                "`http::not_found(...)`": "HTTP API docs must list not_found helper",
                "`http::bad_request(...)`": "HTTP API docs must list bad_request helper",
                "`http::content_too_large()`": "HTTP API docs must list content_too_large helper",
                "`http::gateway_timeout()`": "HTTP API docs must list gateway_timeout helper",
            },
        ),
    )
    failures.extend(
        missing_markers(
            extended,
            {
                "Response blocking_file_response(": "extended facade must keep blocking file response helper",
                "export import conflux.http;": "extended facade must layer on the first-contact facade",
            },
        ),
    )
    failures.extend(
        missing_markers(
            compile_fail,
            {
                "http::file(\"index.html\")": "compile-fail probes must keep file helper out of first-contact facade",
                "auto probe() -> StreamedFile": "compile-fail probes must keep raw streamed-file type out of first-contact globals",
            },
        ),
    )
    failures.extend(
        missing_markers(
            cost_guard,
            {
                "http::ok(": "cost/lifetime guard must reject nonexistent http::ok helper mentions",
                "`http::ok": "cost/lifetime guard must reject backticked nonexistent http::ok helper mentions",
            },
        ),
    )

    if "Response file(" in facade or "http::file(" in snapshot:
        failures.append("first-contact facade must not add http::file; use conflux.http.extended blocking_file_response")
    if "Response ok(" in facade or "http::ok(" in snapshot:
        failures.append("first-contact facade must not advertise nonexistent http::ok helper")

    if failures:
        print("HTTP response helper policy guard failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("HTTP response helper policy: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
