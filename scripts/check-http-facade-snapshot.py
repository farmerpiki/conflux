#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SNAPSHOTS = [
    ROOT / "tests" / "http_facade_api_snapshot.cxx",
    ROOT / "tests" / "http_facade_extended_api_snapshot.cxx",
]
REQUIRED_MARKERS = {
    ROOT / "tests" / "http_facade_api_snapshot.cxx": {
        "http::app()": "public facade snapshot must cover the App factory",
        "http::RequestView const &": "public facade snapshot must cover borrowed request views",
        'http::Path<"id"': "public facade snapshot must cover path extractors",
        "http::QueryParams": "public facade snapshot must cover aggregate query extractors",
        "http::FormParams": "public facade snapshot must cover aggregate form extractors",
        "http::JsonDocument": "public facade snapshot must cover JSON document extraction",
        "http::Json<Payload>": "public facade snapshot must cover typed JSON request/response wrappers",
        "http::Result<http::CreatedBody": "public facade snapshot must cover fallible typed responses",
        "http::Config::public_server()": "public facade snapshot must cover safe config presets",
        "app.validate().detailed_summary()": "public facade snapshot must cover validation diagnostics",
        "app.route_table()": "public facade snapshot must cover route introspection",
        "app.openapi_spec()": "public facade snapshot must cover OpenAPI introspection",
        "http::text(": "public facade snapshot must cover response helpers",
        "http::owned_text(": "public facade snapshot must cover owned response helpers",
        "http::redirect(": "public facade snapshot must cover redirect helpers",
        "http::buffered_stream(": "public facade snapshot must cover streaming response helpers",
        "http::cookie(": "public facade snapshot must cover cookie builders",
    },
    ROOT / "tests" / "http_facade_extended_api_snapshot.cxx": {
        "http::Task<http::Response>": "extended facade snapshot must cover task handler aliases",
        "http::Next const &": "extended facade snapshot must cover sync middleware aliases",
        "http::AsyncNext const &": "extended facade snapshot must cover async middleware aliases",
        "http::offload(": "extended facade snapshot must cover explicit offload helpers",
        "http::defer(": "extended facade snapshot must cover explicit defer helpers",
    },
}
ALLOWED_IMPORTS = {
    "std",
    "conflux.http",
    "conflux.http.extended",
}
FORBIDDEN_IMPORT_PREFIXES = (
    "conflux.net.",
    "conflux.uring",
    "conflux.socket_io",
    "conflux.file_io",
)
IMPORT_RE = re.compile(r"^\s*import\s+([A-Za-z0-9_.:]+)\s*;", re.MULTILINE)


def main() -> int:
    failures: list[str] = []
    for path in SNAPSHOTS:
        text = path.read_text(encoding="utf-8")
        for module in IMPORT_RE.findall(text):
            if module in ALLOWED_IMPORTS:
                continue
            label = "internal module" if module.startswith(FORBIDDEN_IMPORT_PREFIXES) else "non-facade module"
            failures.append(
                f"{path.relative_to(ROOT)} imports {label} `{module}`; "
                "HTTP facade snapshots must exercise only facade imports"
            )
        for marker, message in REQUIRED_MARKERS[path].items():
            if marker not in text:
                failures.append(f"{path.relative_to(ROOT)}: {message}")

    if failures:
        print("http-facade snapshot guard failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("http-facade snapshot guard: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
