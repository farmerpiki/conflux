#!/usr/bin/env python3
"""Cheap guard for App route metadata and OpenAPI strictness docs."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

APP_ROUTE_METADATA_FIELDS = [
    "method",
    "path",
    "name",
    "handler_kind",
    "source_file",
    "source_line",
    "extractors",
    "path_params",
    "required_states",
    "consumes",
    "produces",
    "request_body_schema",
    "response_schema",
    "success_status",
    "max_body_size",
    "middleware_count",
    "openapi_summary",
]

DOC_MARKERS = [
    "## OpenAPI / Route metadata",
    "auto routes = app.routes();",
    "auto statics = app.static_mounts();",
    "auto table = app.route_table();",
    "auto spec = app.openapi_spec();",
    "AppRouteInfo: source, extractors, limits, auth, schemas",
    "AppStaticMountInfo: prefix, root, source location",
    "ValidationReport::detailed_summary()",
    "Stable diagnostic codes",
    "OpenAPI 3.x JSON document",
]

STRICT_MARKERS = [
    "OpenAPI strict mode: route operationId is missing",
    "OpenAPI strict mode: route summary is missing",
    "OpenAPI strict mode: route response content metadata is missing",
    "OpenAPI strict mode: route request body content metadata is missing",
]

TEST_MARKERS = {
    "tests/http_facade_routes_test.cxx": [
        "app.route_table()",
        "middleware_count",
        "source_file.ends_with",
        "openapi_summary",
        "max_body_size",
    ],
    "tests/http_facade_openapi_test.cxx": [
        "openapi_strict",
        "OpenAPI strict mode: route operationId is missing",
        "OpenAPI strict mode: route summary is missing",
        "OpenAPI strict mode: route response content metadata is missing",
        "OpenAPI strict mode: route request body content metadata is missing",
        "operationId",
        "summary",
        "response_schema",
        "requestBody",
    ],
    "tests/http_facade_validation_test.cxx": [
        "source_file.ends_with",
        "related_source_file.ends_with",
    ],
    "tests/http_facade_api_snapshot.cxx": [
        "std::vector<http::AppRouteInfo>",
        "app.route_table()",
    ],
}


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    app = read("src/net/app.cxx")
    runtime = read("src/net/app_runtime.cxx")
    http_docs = read("docs/http-server-api.md")

    for field in APP_ROUTE_METADATA_FIELDS:
        if field not in app:
            failures.append(f"src/net/app.cxx missing route metadata field {field}")

    for marker in DOC_MARKERS:
        if marker not in http_docs:
            failures.append(f"docs/http-server-api.md missing {marker!r}")

    for marker in STRICT_MARKERS:
        if marker not in runtime:
            failures.append(f"src/net/app_runtime.cxx missing {marker!r}")

    for path, markers in TEST_MARKERS.items():
        text = read(path)
        for marker in markers:
            if marker not in text:
                failures.append(f"{path} missing {marker!r}")

    for marker in [
        "`app.validate()` returns source locations",
        "method",
        "path",
        "source_file",
        "source_line",
    ]:
        joined = "\n".join([http_docs, runtime])
        if marker not in joined:
            failures.append(f"route/OpenAPI docs or validation missing {marker!r}")

    if failures:
        print("route/openapi docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("route/openapi docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
