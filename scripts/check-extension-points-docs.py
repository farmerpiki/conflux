#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    docs = read("docs/extension-points.md")
    readme = read("README.md")
    docs_index = read("docs/README.md")
    release_skus = read("docs/release-skus.json")

    columns = [
        "Extension point",
        "Preview status",
        "Public API",
        "Minimal example",
        "Evidence",
    ]
    rows = [
        "JSON codec",
        "Body encoder/decoder",
        "Auth validator",
        "DB pool",
        "Logging sink",
        "Metrics sink",
        "Tracing propagation",
        "TLS provider",
        "Static-file cache policy",
        "Allocator/arena",
        "Runtime/executor integration",
    ]
    markers = [
        "JsonMembers<T>",
        "JsonCodec<T>",
        "try_response_with<Provider>",
        "json_body<T>",
        "basic_auth_middleware(validator)",
        "bearer_auth_middleware(validator)",
        "conflux::pg::Pool",
        "make_access_log_middleware(sink)",
        "ObservabilitySinks",
        "tracing_middleware(TracingOptions)",
        "Fixed provider in preview",
        "StaticFileCacheConfig",
        "JsonArena::parse_into",
        "async_run_on(...)",
        "examples/advanced/custom_json_provider.cxx",
        "examples/public/middleware.cxx",
        "examples/advanced/db_pool.cxx",
        "examples/advanced/http_observability.cxx",
        "examples/quickstart/static_files.cxx",
        "examples/advanced/explicit_offload.cxx",
        "tests/http_facade_observability_test.cxx",
        "tests/api_surface_extended_import_smoke.cxx",
    ]
    linked_docs = [
        "docs/json-api.md",
        "docs/json-boundary-guide.md",
        "docs/http-server-api.md",
        "docs/auth-rate-limit-hooks.md",
        "docs/auth-password-hashing.md",
        "docs/db-api.md",
        "docs/observability.md",
        "docs/configuration.md",
        "docs/cost-lifetime-model.md",
        "docs/conflux-work-root-api.md",
        "docs/execution-model.md",
    ]

    for marker in columns + markers + linked_docs:
        if marker not in docs:
            failures.append(f"docs/extension-points.md missing {marker!r}")
    for row in rows:
        if f"| {row} |" not in docs:
            failures.append(f"docs/extension-points.md missing row {row!r}")
    for path in [
        "examples/advanced/custom_json_provider.cxx",
        "examples/public/middleware.cxx",
        "examples/advanced/db_pool.cxx",
        "examples/advanced/http_observability.cxx",
        "examples/quickstart/static_files.cxx",
        "examples/advanced/explicit_offload.cxx",
        "tests/http_facade_observability_test.cxx",
        "tests/api_surface_extended_import_smoke.cxx",
    ] + linked_docs:
        if not (ROOT / path).exists():
            failures.append(f"extension point target missing {path}")
    if "docs/extension-points.md" not in readme:
        failures.append("README.md must link docs/extension-points.md")
    if "docs/extension-points.md" not in docs_index:
        failures.append("docs/README.md must link docs/extension-points.md")
    if "docs/extension-points.md" not in release_skus:
        failures.append("release SKU docs must include docs/extension-points.md")

    if failures:
        print("extension point docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("extension point docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
