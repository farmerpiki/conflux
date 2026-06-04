#!/usr/bin/env python3
"""Cheap guard for HTTP lifecycle/backpressure docs."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CHECKS = {
    ROOT / "docs" / "http-server-api.md": [
        "Lifecycle and pressure",
        "DrainOptions",
        "DrainReport",
        "OverflowPolicy",
        "HttpPressureMetrics",
        "The preview lifecycle API is intentionally small",
        "`port()` is the readiness wait",
        "There are no separate public `listen()`, `ready()`,",
        "Application background tasks and certificate reload controllers",
    ],
    ROOT / "docs" / "execution-model.md": ["Server drain"],
    ROOT / "docs" / "cost-lifetime-model.md": ["HttpServer::drain"],
    ROOT / "docs" / "release-checklist.md": ["Lifecycle/backpressure docs"],
    ROOT / "docs" / "production-checklist.md": [
        "Lifecycle",
        "Pressure",
        "listen readiness",
        "App::validate()",
        "certificate reload controllers",
    ],
    ROOT / "docs" / "db-api.md": ["PoolConfig::acquire_timeout", "application-owned"],
    ROOT / "docs" / "conflux-work-root-api.md": ["WorkPoolOptions", "enqueue(job) == false"],
}

OVERFLOW_POLICIES = [
    "reject",
    "drop_oldest",
    "drop_newest",
    "close_connection",
    "backpressure",
]

PRESSURE_COUNTERS = [
    "accept_rejected",
    "connections_closed_for_pressure",
    "response_backpressure_events",
    "sse_dropped_newest",
    "sse_dropped_oldest",
    "sse_disconnected_for_pressure",
    "websocket_closed_for_pressure",
    "drain_started",
    "drain_deadline_hit",
    "drain_forced_close",
]

BOUNDARY_MARKERS = [
    "Accept/admission",
    "Request body",
    "Response send",
    "SSE channel",
    "WebSocket outbound",
    "Worker/offload pool",
    "DB pool, when enabled",
    "Ring CQ overflow",
]

BOUNDARY_POLICY_MARKERS = [
    "OverflowPolicy::reject",
    "OverflowPolicy::backpressure",
    "SseOverflowPolicy",
    "websocket_closed_for_pressure",
    "WorkPoolOptions",
    "PoolConfig::acquire_timeout",
    "cq_overflow",
]


def main() -> int:
    failures: list[str] = []
    for path, needles in CHECKS.items():
        if not path.exists():
            failures.append(f"missing {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        for needle in needles:
            if needle not in text:
                failures.append(f"{path.relative_to(ROOT)} missing {needle!r}")

    example = ROOT / "examples" / "advanced" / "http_lifecycle.cxx"
    if not example.exists():
        failures.append("missing examples/advanced/http_lifecycle.cxx")

    server_types = (ROOT / "src" / "net" / "server_types.cxx").read_text(encoding="utf-8")
    http_api = (ROOT / "docs" / "http-server-api.md").read_text(encoding="utf-8")
    production = (ROOT / "docs" / "production-checklist.md").read_text(encoding="utf-8")
    metrics = (ROOT / "src" / "net" / "metrics.cxx").read_text(encoding="utf-8")
    observability = (ROOT / "src" / "net" / "observability.cxx").read_text(encoding="utf-8")
    e2e_tests = (ROOT / "tests" / "E2ETests.cmake").read_text(encoding="utf-8")
    db_api = (ROOT / "docs" / "db-api.md").read_text(encoding="utf-8")
    work_api = (ROOT / "docs" / "conflux-work-root-api.md").read_text(encoding="utf-8")

    for policy in OVERFLOW_POLICIES:
        if policy not in server_types:
            failures.append(f"src/net/server_types.cxx missing OverflowPolicy::{policy}")
        if policy not in http_api:
            failures.append(f"docs/http-server-api.md missing OverflowPolicy::{policy}")

    for counter in PRESSURE_COUNTERS:
        if counter not in server_types:
            failures.append(f"src/net/server_types.cxx missing pressure counter {counter}")
        if counter not in http_api:
            failures.append(f"docs/http-server-api.md missing pressure counter {counter}")
        if counter not in metrics and counter not in observability:
            failures.append(f"pressure metric formatting missing counter {counter}")

    for marker in BOUNDARY_MARKERS:
        if marker not in production:
            failures.append(f"docs/production-checklist.md missing pressure boundary {marker!r}")

    for marker in BOUNDARY_POLICY_MARKERS:
        if marker not in production:
            failures.append(f"docs/production-checklist.md missing pressure policy marker {marker!r}")

    for marker in [
        "application-owned",
        "min_connections",
        "max_connections",
        "acquire_timeout",
    ]:
        if marker not in db_api:
            failures.append(f"docs/db-api.md missing DB pressure marker {marker!r}")

    for marker in [
        "application-owned",
        "max_inject_queue",
        "local_queue_capacity",
        "enqueue(job) == false",
        "work_pool_rejected_total",
        "work_pool_queue_depth",
    ]:
        if marker not in work_api:
            failures.append(f"docs/conflux-work-root-api.md missing work pressure marker {marker!r}")

    for marker in [
        "SseOverflowPolicy",
        "DropNewest",
        "DropOldest",
        "Disconnect",
        "`drop_newest`, `drop_oldest`, and `close_connection`",
        "work_pool_rejected_total",
        "work_pool_queue_depth",
    ]:
        joined = "\n".join([http_api, observability])
        if marker not in joined:
            failures.append(f"lifecycle/backpressure docs missing {marker!r}")

    for marker in [
        "add_executable(conflux_http_backpressure_e2e)",
        "add_executable(conflux_http_full_drain_contract_e2e)",
    ]:
        if marker not in e2e_tests:
            failures.append(f"tests/E2ETests.cmake missing {marker}")

    if failures:
        print("lifecycle docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("lifecycle docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
