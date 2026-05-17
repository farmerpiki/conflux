# TODO / Proposal State Index

Date: 2026-05-16

Purpose: keep branch selection from being driven by stale proposal wording. Treat
this file plus `todo/parallel_priority_plan.md` as the current source of truth.
Older proposal files remain useful design notes, but their headers/status must be
checked against this index before implementation starts.

## Open implementation lanes worth starting

| Priority | Branch | Source doc | Current decision | Why now / why not |
|---|---|---|---|---|
| P1 | `db/pipeline-live-evidence` | `todo/db_remaining.md`, `benchmarks/notes/db_pipeline_live_evidence.md` | Wrapper/summary shape done; run host evidence next. | DB pipeline path still needs real PostgreSQL artifacts before promotion claims. |
| P1 | `http/send-threshold-bench` | `proposals/t1_a_send_zc_proposal.md`, `todo/parallel_priority_plan.md` | Tooling done; run host evidence before tuning defaults. | One-command threshold evidence wrapper and summary exist; threshold policy still needs realistic host artifacts before any default change. |
| P2 | `http/server-impl-split` | `proposals/http_server_impl_split_proposal.md` | Worth implementing as private implementation-unit split. | Improves review/build ergonomics; avoid while recv/server lifetime work is red or unverified. |
| P2 | `json/impl-unit-split` | `proposals/json_module_split_proposal.md` | Worth implementing as zero-behavior source split. | Large primary BMI; do after public alias/API churn settles, or on a branch with no API rename work. |
| P2 | `worker/queue-contention-measurement` | `todo/parallel_priority_plan.md`, `todo/server_gaps.md` | Tooling done; run evidence before changing locks. | Queue/lock probes and a one-command evidence wrapper exist; Chase-Lev/admission rewrites remain unproven until host artifacts show contention. |

## Historical / implemented proposals

| Doc | Current state | Implementation guidance |
|---|---|---|
| `build/bench-regression-budget` | Implemented. | DB-backed per-benchmark budgets, `bench_budget_eval`, and `scripts/bench_check_budget.py` are the merge-gate shape; future work should tune budgets from host data, not rework the gate. |
| `proposals/file_io_module_split_proposal.md` | Implemented as a source-shape split inside the existing `conflux_file_io` target. | Leaf modules are `conflux.file_io.buffers`, `.pipe_pool`, `.reader`, `.iopoll`, and `.driver`; keep `conflux.file_io` as umbrella until consumers have migrated. |
| `proposals/modular_build_targets_proposal.md` | Superseded by `.updated.md` and implemented target graph. | Do not follow old monolith/problem text as open work. Use package/component docs and `CMakeLists.txt`. |
| `proposals/modular_build_targets_proposal.updated.md` | Implemented graph plus remaining coupling notes. | Historical target-boundary rationale. New work should be specific component polish, not another broad graph rewrite. |
| `proposals/conflux_no_std_streams_proposal.md` | Superseded by `.updated.md` for source state. | Keep as rationale only; stream-vocabulary removal is complete. |
| `proposals/conflux_no_std_streams_proposal.updated.md` | Implemented, including diagnostic-print policy. | Direct reusable-source `std::print/std::println(stderr, ...)` is disallowed; the guard enforces source cleanliness while tests/examples/benchmarks keep human-facing `std::println`. |
| `proposals/p1_02b_https_async_cancel_proposal.md` | Implemented. | Keep for cancellation design rationale/tests. |
| `proposals/p1_08b_cancel_by_fd_proposal.md` | Implemented. | Keep for recv-close generation rationale/tests. |
| `todo/p1_09a_tcp_accept_async_proposal.md` | Implemented. | Keep historical; async accept API is not open work. |
| `todo/p1_09_socket_task_ring_bench_proposal.md` | Implemented for intended benchmark coverage. | Any remaining work is measurement/evidence, not missing API surface. |
| `proposals/p2_g_registered_send_buffers_proposal.md` | Implemented. | Follow-up only if HTTP send benchmarks prove more direct-format work is needed. |
| `proposals/t1_a_send_zc_proposal.md` | Implemented for plain/mapped path; TLS intentionally unchanged. | Remaining decision is threshold benchmarking, not SEND_ZC plumbing. |
| `proposals/t2_c_iopoll_ring_proposal.md` | Storage-only primitive implemented; HTTP/static adoption benchmark-gated. | Do not wire into HTTP static path until storage-read bottleneck is measured. |

## Gaps found in current docs

1. **Branch-source ambiguity.** `todo/next_priorities.md`, module split proposals,
   and `todo/parallel_priority_plan.md` all name plausible next branches, but only
   the parallel plan accounts for latest P0/P1 gates. Fix: use this index for
   proposal status, then the parallel plan for branch selection.
2. **Implemented proposals still look actionable.** Original modular-build,
   stream-removal, P1-09, P1-09a, P2-G, and cancellation docs have useful design
   detail but stale problem statements. Fix: add state notes and keep them out of
   immediate branch fan-out.
3. **Remaining module-split proposals are worthwhile but not blockers.**
   `http_server_impl` and `json` splits improve ergonomics/build locality, but
   neither should preempt SEND_ZC threshold evidence, DB host pipeline evidence,
   benchmark-budget threshold tuning from real artifacts, or recv/server
   correctness verification. The `file_io`
   source-shape split is already landed inside the existing component.
4. **Perf changes still need evidence.** SEND_ZC thresholds, IOPOLL static-path
   adoption, worker queue lock replacement, and ring layout padding must remain
   measurement branches first.

## Practical selection rule

- Pick the first P0/P1 branch in this file unless already working in that
  component.
- When editing file/HTTP/JSON internals anyway, prefer the named module-split
  seam from the relevant proposal and keep public targets/imports unchanged.
- Do not remove aliases or broad shorthand vocabulary until the release cleanup
  lane says the public surface is settled.
