#!/usr/bin/env python3
"""Cheap guard for cancellation semantics docs."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EXECUTION_TABLE_MARKERS = [
    "## Cancellation semantics",
    "| Boundary | Before submit / admission | In flight | After CQE / result | Timeout support |",
    "| User task (`root::Task`, `TaskControl`) |",
    "| Socket read/write (`SocketTaskRing`, HTTP receive/send) |",
    "| TLS |",
    "| `send_zc` and async file-backed send |",
    "| Request body receive |",
    "| Response body send |",
    "| Async file read |",
    "| DNS resolve |",
    "| DB acquire/query |",
    "Cancellation is a request, not proof that already-submitted kernel or provider",
]

API_MARKERS = {
    "docs/conflux-work-root-api.md": [
        "make_cancellable_task_source",
        "install_cancel_hook",
        "request_cancel",
        "fired cancel hook means the source cannot set success",
    ],
    "docs/file-io-api.md": [
        "Cancellation And Timeouts",
        "async_cancel",
        "async_cancel_fd",
        "Ordinary file reads do not carry a hidden per-read deadline",
    ],
    "docs/dns-api.md": [
        "Cancellation And Timeouts",
        "waiter-scoped",
        "Native UDP receive cancellation is\nbest-effort",
        "query_timeout",
        "total_timeout",
    ],
    "docs/db-api.md": [
        "Query cancellation is best effort through libpq",
        "cancel_inflight",
        "QueryOptions::deadline",
        "External cancellation of a queued\nacquire completes the task as cancelled",
    ],
    "docs/conflux-http-client-api.md": [
        "Cancellation is routed through `SocketTaskRing`",
        "already-submitted DNS, connect, TLS, write, and read work",
        "later cancellation does not\nrewrite that result",
        "Write timeout",
        "Cancellation-safe close",
    ],
    "docs/release-checklist.md": [
        "`docs/execution-model.md` cancellation semantics match async task, socket",
        "TLS, `send_zc`, async file, DNS, and DB behavior",
    ],
}


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    execution = read("docs/execution-model.md")
    for marker in EXECUTION_TABLE_MARKERS:
        if marker not in execution:
            failures.append(f"docs/execution-model.md missing {marker!r}")

    for path, markers in API_MARKERS.items():
        text = read(path)
        for marker in markers:
            if marker not in text:
                failures.append(f"{path} missing {marker!r}")

    if failures:
        print("cancellation docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("cancellation docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
