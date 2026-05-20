# TODO / Proposal State Index

Date: 2026-05-16

Purpose: keep branch selection from being driven by stale proposal wording. Treat
this file plus `todo/parallel_priority_plan.md` as the current source of truth.
Older proposal files remain useful design notes, but their headers/status must be
checked against this index before implementation starts.

## Open implementation lanes worth starting

| Priority | Branch | Source doc | Current decision | Why now / why not |
|---|---|---|---|---|
| DONE | `db/pipeline-live-evidence` | `todo/db_remaining.md`, `benchmarks/notes/db_pipeline_live_evidence.md` | Host PostgreSQL evidence captured: 18 DB integration tests passed and pipeline median speedup was 2.33x over plain. | No DB runtime change needed from this evidence; promotion claims can reference the artifact. |
| P1 | `http/send-threshold-bench` | `docs/archive/proposals/2026-05/t1_a_send_zc_proposal.md`, `todo/parallel_priority_plan.md` | Capable-host summary reviewed; no threshold change. | Plain SEND_ZC candidates were all copied, TLS correctly bypassed, and static-file rows labelled `mapped` did not hit mapped SEND_ZC attempts. Next work is benchmark/path coverage cleanup, not default tuning. |
| DONE | `json/impl-unit-split` | `docs/archive/proposals/2026-05/json_module_split_proposal.md` | Parser/arena extraction landed and follow-up source-shape cleanup moved builder lifecycle, DOM lookup/equality, string-token decode, and storage helpers out of the primary module. | Keep public `import conflux.json` and `conflux::json` unchanged; future JSON work should be behavior-specific, not another broad implementation split. |
| DONE | `template/compiled-cache-reload` | `docs/archive/proposals/2026-05/template_compiled_cache_proposal.md` | Compiled reusable templates, structured compile/link diagnostics, checked reload, render-check preflight, parsed-context overloads, and explicit reload semantics implemented. | Further template work should be targeted polish or evidence, not the deferred watcher/header cleanup items. |
| DONE | `worker/queue-contention-measurement` | `todo/parallel_priority_plan.md`, `todo/server_gaps.md` | Host evidence captured; no queue default or lock rewrite. | `no_stealing` is useful for bounded independent offload, but default `stealing` preserves work conservation on local-backlog redistribution. |
| DONE | `release/prerelease-2-day-readiness` | `conflux_prerelease_2_day_agent_proposal.md` | Header-interface package lane, DB-off install pruning, package smoke, example manifest, diagnostics stream cleanup, prerelease known-limitations docs, and release checklist gates landed. | Remaining prerelease work starts from `conflux_prerelease_followup_cleanup_plan.md`; do not reopen the older prerelease proposal as an active blocker. |
| P2 | `json/direct-struct-decode` | `proposals/json_direct_struct_decode_proposal.md` | Open but not prerelease-blocking. | Keep behind the current prerelease package/docs gates; implement only after release-facing docs and evidence are settled. |
| P2 | `uring/iopoll-static-evidence` | `proposals/t2_c_iopoll_ring_proposal.md`, `proposals/perf_ideas.md` | Storage-only primitive is implemented; HTTP/static adoption remains evidence-gated. | Do not wire IOPOLL into HTTP static paths until a same-machine benchmark proves storage-read bottleneck. |
| P2 | `perf/evidence-inventory` | `proposals/perf_ideas.md` | Open inventory, not a direct implementation branch. | Use it to classify benchmark/evidence gaps after checking this state index. |

## Historical / implemented proposals

| Doc | Current state | Implementation guidance |
|---|---|---|
| `build/bench-regression-budget` | Implemented. | DB-backed per-benchmark budgets, `bench_budget_eval`, and `scripts/bench_check_budget.py` are the merge-gate shape; future work should tune budgets from host data, not rework the gate. |
| `docs/archive/proposals/2026-05/file_io_module_split_proposal.md` | Implemented as a source-shape split inside the existing `conflux_file_io` target. | Leaf modules are `conflux.file_io.buffers`, `.pipe_pool`, `.reader`, `.iopoll`, and `.driver`; keep `conflux.file_io` as umbrella until consumers have migrated. |
| `docs/archive/proposals/2026-05/http_server_impl_split_proposal.md` | Implemented as private HTTP server module units. | `http_server_impl.cxx` is now facade glue over private state/recv/send/CQE/loop/dispatch/TLS/H2/WS units; treat further HTTP server work as targeted behavior/evidence, not another broad split. |
| `docs/archive/proposals/2026-05/modular_build_targets_proposal.md` | Superseded by `.updated.md` and implemented target graph. | Do not follow old monolith/problem text as open work. Use package/component docs and `CMakeLists.txt`. |
| `docs/archive/proposals/2026-05/modular_build_targets_proposal.updated.md` | Implemented graph plus remaining coupling notes. | Historical target-boundary rationale. New work should be specific component polish, not another broad graph rewrite. |
| `docs/archive/proposals/2026-05/conflux_no_std_streams_proposal.md` | Superseded by `.updated.md` for source state. | Keep as rationale only; stream-vocabulary removal is complete. |
| `docs/archive/proposals/2026-05/conflux_no_std_streams_proposal.updated.md` | Implemented, including diagnostic-print policy. | Direct reusable-source `std::print/std::println(stderr, ...)` is disallowed; the guard enforces source cleanliness while tests/examples/benchmarks keep human-facing `std::println`. |
| `docs/archive/proposals/2026-05/p1_02b_https_async_cancel_proposal.md` | Implemented. | Keep for cancellation design rationale/tests. |
| `docs/archive/proposals/2026-05/p1_08b_cancel_by_fd_proposal.md` | Implemented. | Keep for recv-close generation rationale/tests. |
| `docs/archive/proposals/2026-05/p1_09a_tcp_accept_async_proposal.md` | Implemented. | Keep historical; async accept API is not open work. |
| `docs/archive/proposals/2026-05/p1_09_socket_task_ring_bench_proposal.md` | Implemented for intended benchmark coverage. | Any remaining work is measurement/evidence, not missing API surface. |
| `docs/archive/proposals/2026-05/p2_g_registered_send_buffers_proposal.md` | Implemented. | Follow-up only if HTTP send benchmarks prove more direct-format work is needed. |
| `docs/archive/proposals/2026-05/t1_a_send_zc_proposal.md` | Implemented for plain/mapped path; TLS intentionally unchanged. | Remaining decision is threshold benchmarking, not SEND_ZC plumbing. |
| `proposals/t2_c_iopoll_ring_proposal.md` | Storage-only primitive implemented; HTTP/static adoption benchmark-gated. | Do not wire into HTTP static path until storage-read bottleneck is measured. |
| `proposals/prerelease_2_day_coding_agent_plan.md` | Implemented by the prerelease readiness branch; active HEADER_INTERFACE, CONFLUX_ENABLE_DB, no_std_streams, and package-smoke checks moved to `docs/release-checklist.md`. | Treat as historical acceptance evidence. Follow-up cleanup starts from `conflux_prerelease_followup_cleanup_plan.md`. |

## Gaps found in current docs

1. **Branch-source ambiguity.** `todo/next_priorities.md`, module split proposals,
   and `todo/parallel_priority_plan.md` all name plausible next branches, but only
   the parallel plan accounts for latest P0/P1 gates. Fix: use this index for
   proposal status, then the parallel plan for branch selection.
2. **Implemented proposals still look actionable.** Original modular-build,
   stream-removal, P1-09, P1-09a, P2-G, and cancellation docs have useful design
   detail but stale problem statements. Fix: add state notes and keep them out of
   immediate branch fan-out.
3. **Remaining module-split work is worthwhile but not blocker-class.**
   `json/impl-unit-split` improves ergonomics/build locality, but it should not
   preempt SEND_ZC path-coverage cleanup, benchmark-budget threshold tuning from
   real artifacts, or recv/server correctness verification.
   The `file_io` and `http_server_impl` source-shape splits are already landed.
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
