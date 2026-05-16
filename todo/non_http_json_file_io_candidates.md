# Non-HTTP / non-JSON / non-file_io proposal candidates

Date: 2026-05-16
Scope: branch candidates from current `todo/` and `proposals/` that do not need
changes under HTTP, JSON, or file I/O implementation paths. Treat
`http/send-threshold-bench`, `http/server-impl-split`, `json/impl-unit-split`, and
`file/file-io-module-split` as separate-branch work and keep them out of these
lanes.

## Best next branches

| Priority | Branch | Worth doing? | Touch set | Acceptance |
|---|---|---|---|---|
| P0 | `build/bench-regression-budget` | Done. Added DB-backed per-benchmark budgets, `bench_budget_eval`, and a merge-gate checker that blocks regressions/noisy/unbudgeted rows while printing artifact paths. | `scripts/bench_check_budget.py`, `scripts/bench_db_migrate.sql`, `benchmarks/README.md`. | Same-machine baseline/candidate comparison has explicit per-benchmark budgets, noisy rows are classified, and merge-blocking output points to recorded artifact paths. |
| P1 | `docs/src-diagnostic-print-policy` | Yes, small ergonomics cleanup. The remaining reusable-source `std::print/std::println(stderr, ...)` use is limited and policy-owned. | `proposals/conflux_no_std_streams_proposal.updated.md`, `scripts/check_no_std_streams.py`, possibly `src/utils.cxx` and `src/work/carrier_coro.cxx`. | Policy says whether reusable-source `std::print/std::println(stderr, ...)` stays allowed. If disallowed, source cleanup avoids adding a `conflux_work -> conflux_utils` dependency just to call `eprint`. |
| P1 | `db/pipeline-live-evidence` | Yes, but evidence-only. The wire-mode path is implemented; promotion needs host-local PostgreSQL numbers, not API redesign. | `benchmarks/notes/db_pipeline_live_evidence.md`, generated evidence artifacts, optional `docs/release-checklist.md` note. | `scripts/db_pipeline_live_evidence.sh` output includes DB integration log, raw NDJSON, summary JSON, and median/best pipeline speedup for repeated runs. |
| P1 | `build/stale-tcp-parallel-bench-prune` | Done. Deleted the obsolete standalone bench and pruned recorder/docs support for its custom parser; N=4 coverage lives in `tcp_increment_coro_bench`. | `benchmarks/CMakeLists.txt`, `benchmarks/README.md`, `scripts/bench_record.sh`, `benchmarks/tcp_parallel_coro_bench.cxx`. | No stale `co_spawn` bench TODO remains; recorder docs/scripts no longer carry an unused `tcp_parallel` parser. |
| P2 | `worker/queue-contention-measurement` | Done for tooling. Added a one-command queue-contention evidence wrapper plus a reusable NDJSON summary validator; scheduler locks remain unchanged pending host measurements. | `scripts/work_queue_contention_evidence.sh`, `scripts/work_queue_contention_summary.py`, `benchmarks/notes/worker_queue_contention_profile.md`, `benchmarks/README.md`. | Host runs now produce configure/build logs, raw NDJSON, manifest JSON, and summary JSON with admission/local/steal/futex rates per 1k jobs. |
| P2 | `worker/root-split-evidence` | Later. `src/work/root.cxx` is large enough to split, but TODO explicitly requires profiling first. | `src/work/root.cxx`, new private work implementation units, `CMakeLists.txt`, tests. | Only start after allocation/control-block hot paths are profiled; preserve allocation counters and pooled coroutine-frame behavior. |

## Defer / skip from this filtered scope

- `db/copy-api`: useful eventually, but `todo/db_remaining.md` says no in-tree
  consumer yet. Defer until COPY has a real caller or benchmark target priority.
- `db/query-stream`: blocked on a worker multi-shot stream/channel primitive. Do
  not invent a DB-local stream shape.
- `worker/when-all-fast-fail`: current docs correctly mark it as identical to
  `when_all` until an async carrier path exists. Do not patch semantics locally.
- `uring/recv-zc`: non-HTTP enough in theory, but still kernel-maturity gated.
- `release/remove-aliases`: final cleanup only; do not mix into any of the above.
- `uring/ring-layout-c2c-verify`: current notes tie this to HTTP server `Ring`
  layout, so exclude it from the no-HTTP branch set.

## Practical ordering

1. Start `build/bench-regression-budget`.
2. In parallel, do `docs/src-diagnostic-print-policy` because it is tiny and low
   conflict.
3. Run `db/pipeline-live-evidence` on a PostgreSQL host.
4. DONE: prune the disabled standalone TCP parallel bench.
5. DONE: add one-command WorkPool queue-contention evidence capture.
6. Only after recorded evidence, decide whether worker lock replacement or
   `work/root.cxx` splitting is worth implementation.
