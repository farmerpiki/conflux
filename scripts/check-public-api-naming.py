#!/usr/bin/env python3
"""Guard canonical public API names for first-contact HTTP docs/examples."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TEXT_PATHS = [
    ROOT / "README.md",
    ROOT / "docs" / "http-server-api.md",
    ROOT / "docs" / "public-api-map.md",
    ROOT / "docs" / "component-map.md",
    ROOT / "docs" / "cost-lifetime-model.md",
    ROOT / "examples" / "quickstart",
    ROOT / "examples" / "public",
]

CANONICAL_MARKERS = {
    "tests/http_facade_api_snapshot.cxx": [
        "http::RequestView const &",
        "http::Config::public_server()",
        "http::text(",
        "http::created(Payload",
        "http::no_content()",
    ],
    "docs/http-server-api.md": [
        "`http::Request` / `http::RequestView`: zero-copy request view",
        "`http::OwnedRequest`: explicit owned copy",
        "accept `http::RequestView` or typed",
        "Return\n`conflux::work::Task<http::Response>` only when",
        "class Response",
        "static Response no_content();",
    ],
    "docs/public-api-map.md": [
        "import conflux.http;",
        "import conflux.json;",
        "import conflux.extended;",
        "import conflux.complete;",
    ],
}

FORBIDDEN_PUBLIC_SPELLINGS = [
    "HttpRequestView",
    "HttpResponse",
    "ServerConfig",
    "http::Task",
    "http::Next",
    "http::AsyncNext",
    "json_response(",
    "http::ok(",
]

FORBIDDEN_EXAMPLE_IMPORTS = [
    "import conflux.types;",
    "import conflux.http.extended;",
    "import conflux.extended;",
]

SHORT_ALIAS_RE = re.compile(
    r"\busing\s+(?:S|SV|SP|UP|Opt|Fn|Tup|RE|EC|SZ)\b"
    r"|\bhttp::(?:S|SV|SP|UP|Opt|Fn|Tup|RE|EC|SZ)\b"
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def text_files() -> list[Path]:
    found: list[Path] = []
    for path in TEXT_PATHS:
        if path.is_file():
            found.append(path)
        else:
            found.extend(p for p in path.rglob("*") if p.is_file())
    return sorted(found)


def main() -> int:
    failures: list[str] = []

    for relpath, markers in CANONICAL_MARKERS.items():
        text = read(ROOT / relpath)
        for marker in markers:
            if marker not in text:
                failures.append(f"{relpath}: missing canonical naming marker {marker!r}")

    for path in text_files():
        try:
            text = read(path)
        except UnicodeDecodeError:
            continue
        relpath = path.relative_to(ROOT)
        for spelling in FORBIDDEN_PUBLIC_SPELLINGS:
            if spelling in text:
                failures.append(f"{relpath}: public docs/examples must not advertise {spelling}")
        if path.suffix == ".cxx" and (ROOT / "examples") in path.parents:
            for spelling in FORBIDDEN_EXAMPLE_IMPORTS:
                if spelling in text:
                    failures.append(f"{relpath}: public examples must not import {spelling}")
            for line_no, line in enumerate(text.splitlines(), start=1):
                if SHORT_ALIAS_RE.search(line):
                    failures.append(f"{relpath}:{line_no}: public examples must not use shorthand type aliases")

    if failures:
        print("public API naming guard failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("public API naming: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
