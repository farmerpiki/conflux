#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    docs = read("docs/observability.md")
    http_docs = read("docs/http-server-api.md")
    impl = read("src/net/observability.cxx")
    tests = read("tests/http_facade_observability_test.cxx")
    fragment = read("tests/HttpObservabilityTests.cmake")

    required_docs = [
        "app.use(conflux::http::observability({",
        "request ID header propagation with `X-Request-ID`",
        "W3C `traceparent` propagation",
        "one structured JSON access log event per request",
        "Prometheus text metrics at `/metrics`",
        "route-pattern labels, with `<unmatched>` for 404s",
        "sensitive-header redaction",
        "http_requests_total{service,route,method,status_class,status}",
        "http_request_duration_seconds_bucket{service,route,method,le}",
        "http_rejections_total{service,reason,status}",
        "Query strings are never metric labels",
        "Header logging is also off by default",
        "request_id_middleware",
        "tracing_middleware",
        "structured_log_middleware",
        "metrics_middleware",
        "observability_server_hooks()",
        "Server pressure, work-pool, task-allocation, and JSON-arena metrics are only\nemitted when the app supplies an explicit source",
        "work_pool_queue_stats_enabled",
        "CONFLUX_WORK_QUEUE_STATS",
    ]
    required_http_docs = [
        "`http::observability()` is the app-facing facade",
        "`register_metrics_route = false`",
        "query\nstrings are never metric labels",
        "Structured logs redact `Authorization`",
        "`Proxy-Authorization`, `Cookie`, `Set-Cookie`, `X-Api-Key`, `X-Api-Token`",
        "ObservabilitySinks{.pressure_metrics = [&server] { return",
        "Work-pool samples include `work_pool_queue_stats_enabled`",
        "Streaming\nresponses currently measure response creation/header commit duration",
    ]
    required_impl = [
        "struct ObservabilityOptions",
        "bool access_log = true",
        "bool request_id = true",
        "bool trace_context = true",
        "bool route_latency = true",
        "bool route_request_count = true",
        "bool rejection_metrics = true",
        "bool pressure_metrics = true",
        "bool register_metrics_route = true",
        "std::function<HttpPressureMetrics()> pressure_metrics_source",
        '"authorization"',
        '"proxy-authorization"',
        '"cookie"',
        '"set-cookie"',
        '"x-api-key"',
        '"x-api-token"',
        '"x-auth-token"',
        '"x-csrf-token"',
        "http_request_duration_seconds",
        "http_rejections_total",
        "work_pool_queue_stats_enabled",
        "ObservabilityMiddleware observability(",
    ]
    required_tests = [
        "http::observability({",
        "X-Request-ID",
        "Traceparent",
        "/request_headers/Authorization",
        "<redacted>",
        "http_request_duration_seconds_count",
        "http_rejections_total",
        "work_pool_queue_stats_enabled",
        "pressure_metrics = [pressure]",
        "register_metrics_route = false",
    ]
    required_fragment = [
        "conflux_http_middleware_compile_fail_global_request_id_options",
        "conflux_http_middleware_compile_fail_global_tracing_middleware",
        "conflux_http_metrics_compile_fail_global_counter",
        "conflux_http_metrics_compile_fail_global_pressure_formatter",
    ]

    checks = [
        ("docs/observability.md", docs, required_docs),
        ("docs/http-server-api.md", http_docs, required_http_docs),
        ("src/net/observability.cxx", impl, required_impl),
        ("tests/http_facade_observability_test.cxx", tests, required_tests),
        ("tests/HttpObservabilityTests.cmake", fragment, required_fragment),
    ]
    for path, text, markers in checks:
        for marker in markers:
            if marker not in text:
                failures.append(f"{path} missing {marker!r}")

    if failures:
        print("observability docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("observability docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
