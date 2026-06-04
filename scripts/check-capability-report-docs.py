#!/usr/bin/env python3
"""Guard the prerelease capability-report example contract."""
from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    example = read("examples/advanced/capability_report.cxx")
    module_examples = read("examples/CMakeLists.txt")
    header_examples = read("cmake/ConfluxInterfaceMode.cmake")
    configuration = read("docs/configuration.md")
    release_skus = json.loads(read("docs/release-skus.json"))

    example_markers = [
        "import conflux.types;",
        "import conflux.uring;",
        "conflux::build_info_summary()",
        "conflux::runtime::detect_capabilities()",
        "conflux::runtime::capability_report(*caps)",
        "conflux::runtime::capability_issue_code_string(issue.code)",
    ]
    for marker in example_markers:
        if marker not in example:
            failures.append(f"examples/advanced/capability_report.cxx missing {marker!r}")

    module_markers = [
        "conflux_capability_report_example",
        "advanced/capability_report.cxx",
        "target_link_libraries(conflux_capability_report_example PRIVATE conflux conflux_options)",
    ]
    for marker in module_markers:
        if marker not in module_examples:
            failures.append(f"examples/CMakeLists.txt missing {marker!r}")

    header_markers = [
        "conflux_add_header_example_from_id(conflux_capability_report_example examples/advanced/capability_report",
        "IMPLS conflux_header_impl_runtime",
    ]
    for marker in header_markers:
        if marker not in header_examples:
            failures.append(f"cmake/ConfluxInterfaceMode.cmake missing {marker!r}")

    doc_markers = [
        "conflux::runtime::detect_capabilities()",
        "conflux::runtime::capability_report(caps)",
        "conflux::http::HttpServer::startup_report()",
        "examples/advanced/capability_report.cxx",
        "before starting an HTTP service",
    ]
    for marker in doc_markers:
        if marker not in configuration:
            failures.append(f"docs/configuration.md missing {marker!r}")

    web_server = release_skus.get("release-web-server")
    examples = web_server.get("examples") if isinstance(web_server, dict) else None
    if not isinstance(examples, list) or "examples/advanced/capability_report.cxx" not in examples:
        failures.append("release-web-server SKU must include examples/advanced/capability_report.cxx")

    if failures:
        print("capability-report docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("capability-report docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
