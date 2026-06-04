#!/usr/bin/env python3
"""Cheap guard for HTTP security posture docs."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

LIMIT_MARKERS = {
    "kConfigDefaultMaxBodySize": "1048576",
    "kConfigDefaultRequestTimeoutMs": "30000",
    "kConfigDefaultTlsSniffTimeoutMs": "10000",
    "kConfigDefaultMaxRequestLineSize": "8192",
    "kConfigDefaultMaxHeaderLineSize": "8192",
    "kConfigDefaultMaxHeaders": "100",
    "kConfigDefaultMaxHeaderBlockSize": "65536",
    "kConfigDefaultMaxChunks": "100000",
}

REJECTION_REASONS = [
    "request_line_too_large",
    "header_block_too_large",
    "too_many_headers",
    "content_length_with_transfer_encoding",
    "invalid_chunk",
    "body_too_large",
    "header_timeout",
    "body_timeout",
]

POLICY_MARKERS = [
    "Trusted proxy headers",
    "CORS preflight",
    "CSRF double-submit",
    "HSTS header",
    "Secure; HttpOnly; SameSite",
]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def source_constant(text: str, name: str) -> str | None:
    match = re.search(rf"\b{name}\b\s*=\s*([^;\n]+)", text)
    if match is None:
        return None
    value = match.group(1)
    value = value.replace("std::size_t{1024} * 1024", "1048576")
    value = value.replace("std::size_t{8} * 1024", "8192")
    value = value.replace("std::size_t{64} * 1024", "65536")
    return value.strip()


def main() -> int:
    failures: list[str] = []
    config = read("src/net/config.cxx")
    http_docs = read("docs/http-server-api.md")
    production = read("docs/production-checklist.md")
    release = read("docs/release-checklist.md")
    auth = read("docs/auth-session-token-audit.md")
    security_tests = "\n".join(
        read(path)
        for path in [
            "tests/http_policy_e2e.cxx",
            "tests/http_csrf_e2e.cxx",
            "tests/http_security_headers_e2e.cxx",
        ]
    )

    for name, expected in LIMIT_MARKERS.items():
        actual = source_constant(config, name)
        if actual != expected:
            failures.append(f"src/net/config.cxx {name} expected {expected}, got {actual}")

    for marker in [
        "1 MiB request bodies",
        "request timeout",
        "TLS/plain sniff timeout",
        "8 KiB request lines",
        "100 headers",
        "64 KiB aggregate header blocks",
        "max_body_size = 1048576",
        "request_timeout_ms = 30000",
        "tls_sniff_timeout_ms = 10000",
        "max_request_line_size = 8192",
        "max_header_line_size = 8192",
        "max_headers = 100",
        "max_header_block_size = 65536",
        "max_chunks = 100000",
    ]:
        if marker not in http_docs:
            failures.append(f"docs/http-server-api.md missing {marker!r}")

    for reason in REJECTION_REASONS:
        counter = f"rejections.{reason}"
        if reason not in http_docs:
            failures.append(f"docs/http-server-api.md missing rejection reason {reason}")
        if counter not in http_docs:
            failures.append(f"docs/http-server-api.md missing rejection counter {counter}")
        if reason not in release:
            failures.append(f"docs/release-checklist.md missing rejection evidence for {reason}")

    for marker in POLICY_MARKERS:
        joined = "\n".join([production, release, auth, security_tests])
        if marker not in joined:
            failures.append(f"security posture docs/tests missing {marker!r}")

    if "Content-Length with `Transfer-Encoding`" not in production:
        failures.append("docs/production-checklist.md missing request smuggling defense checklist item")

    if failures:
        print("security posture docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("security posture docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
