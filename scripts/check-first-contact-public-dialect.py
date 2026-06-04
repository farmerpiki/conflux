#!/usr/bin/env python3
"""Reject stale public spellings in first-contact docs/examples."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATHS = [
    ROOT / "README.md",
    ROOT / "docs",
    ROOT / "examples" / "quickstart",
]
SKIP = {
    ROOT / "docs" / "archive",
    ROOT / "docs" / "README.md",
    ROOT / "docs" / "planning-policy.md",
    ROOT / "docs" / "naming-audit.md",
    ROOT / "docs" / "io_uring_direct_file_flow_design.md",
}

PATTERN = re.compile(
    r"(?<![-A-Za-z0-9_])(S|SV|SP|UP|Opt|Fn|Tup|RE|EC|SZ)(?![A-Za-z0-9_])"
    r"|(?<![A-Za-z0-9_])(?:send_async|proxy_async|dispatch_async|block_on_socket_task|write_all_fd|read_all_fd)(?![A-Za-z0-9_])"
    r"|Config::benchmark\(\)"
    r"|req\.params\[[^\]]+\]"
)
QUICKSTART_ALLOWED_LEAF_IMPORTS = {
    "import conflux.json.reflect;",
}


def check_http_server_api_order(failures: list[str]) -> None:
    path = ROOT / "docs" / "http-server-api.md"
    text = path.read_text(encoding="utf-8")
    markers = [
        "## Typed routes and extractors",
        "## Handlers",
        "## Extended router reference",
    ]
    positions = [text.find(marker) for marker in markers]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        failures.append(
            "docs/http-server-api.md: first-contact route and handler sections must precede extended router reference",
        )
    handler_markers = [
        "The first-contact handler mental model is",
        "return `http::Response`",
        "Return\n`conflux::work::Task<http::Response>` only when the handler has real suspension\npoints.",
        "Task-returning handler: use only when the work has suspension points.",
    ]
    missing = [marker for marker in handler_markers if marker not in text]
    if missing:
        failures.append("docs/http-server-api.md: missing first-contact handler mental-model wording")


def files() -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for path in PATHS:
        if path.is_file():
            found.append(path)
        else:
            found.extend(p for p in path.rglob("*") if p.is_file())
    return sorted(found)


def skipped(path: pathlib.Path) -> bool:
    return any(path == item or item in path.parents for item in SKIP)


def main() -> int:
    failures: list[str] = []
    check_http_server_api_order(failures)
    for path in files():
        if skipped(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for line_no, line in enumerate(text.splitlines(), start=1):
            stripped = line.strip()
            if "cmake -S" in line or stripped.startswith(("-S ", "--source ")):
                continue
            if ("todo/" in line or "proposals/" in line) and "benchmarks/README.md" not in line:
                rel = path.relative_to(ROOT)
                failures.append(f"{rel}:{line_no}: first-contact docs must not route users through planning files")
            if PATTERN.search(line):
                rel = path.relative_to(ROOT)
                failures.append(f"{rel}:{line_no}: {line.strip()}")
            if ROOT / "examples" / "quickstart" in path.parents:
                if stripped.startswith("import conflux.") and stripped not in QUICKSTART_ALLOWED_LEAF_IMPORTS:
                    rel = path.relative_to(ROOT)
                    failures.append(
                        f"{rel}:{line_no}: quickstart examples must not import advanced leaves: {stripped}",
                    )

    if failures:
        print("first-contact public dialect check failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
