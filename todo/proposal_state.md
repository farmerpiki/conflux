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
| P1 | `http/send-threshold-bench` | `docs/archive/proposals/README.md`, `todo/parallel_priority_plan.md` | Capable-host summary reviewed; no threshold change. | Plain SEND_ZC candidates were all copied, TLS correctly bypassed, and static-file rows labelled `mapped` did not hit mapped SEND_ZC attempts. Next work is benchmark/path coverage cleanup, not default tuning. |
| DONE | `json/impl-unit-split` | `docs/archive/proposals/README.md` | Parser/arena extraction landed and follow-up source-shape cleanup moved builder lifecycle, DOM lookup/equality, string-token decode, and storage helpers out of the primary module. | Keep public `import conflux.json` and `conflux::json` unchanged; future JSON work should be behavior-specific, not another broad implementation split. |
| DONE | `template/compiled-cache-reload` | `docs/archive/proposals/README.md` | Compiled reusable templates, structured compile/link diagnostics, checked reload, render-check preflight, parsed-context overloads, and explicit reload semantics implemented. | Further template work should be targeted polish or evidence, not the deferred watcher/header cleanup items. |
| DONE | `worker/queue-contention-measurement` | `todo/parallel_priority_plan.md`, `todo/server_gaps.md` | Host evidence captured; no queue default or lock rewrite. | `no_stealing` is useful for bounded independent offload, but default `stealing` preserves work conservation on local-backlog redistribution. |
| DONE | `release/prerelease-2-day-readiness` | `conflux_prerelease_2_day_agent_proposal.md` | Header-interface package lane, DB-off install pruning, package smoke, example manifest, diagnostics stream cleanup, prerelease known-limitations docs, and release checklist gates landed. | Remaining prerelease work starts from `conflux_prerelease_followup_cleanup_plan.md`; do not reopen the older prerelease proposal as an active blocker. |
| DONE | `release/modules-first-artifacts` | `docs/package-consumption.md`, `docs/prerelease-status.md`, `scripts/stage-release-artifacts.sh` | Modules-first preview contract landed: `MODULE_INTERFACE` is primary, generated headers are staged release artifacts, bridge manifests include Python/options metadata, and mock liburing is internal compile evidence only. | Future release work should attach artifact evidence, not reintroduce generated headers as tracked source or the primary consumer story. |
| P2 | `json/direct-struct-decode` | `proposals/json_direct_struct_decode_proposal.md` | Open but not prerelease-blocking. | Keep behind the current prerelease package/docs gates; implement only after release-facing docs and evidence are settled. |
| P2 | `uring/iopoll-static-evidence` | `proposals/t2_c_iopoll_ring_proposal.md`, `proposals/perf_ideas.md` | Storage-only primitive is implemented; HTTP/static adoption remains evidence-gated. | Do not wire IOPOLL into HTTP static paths until a same-machine benchmark proves storage-read bottleneck. |
| P2 | `perf/evidence-inventory` | `proposals/perf_ideas.md` | Open inventory, not a direct implementation branch. | Use it to classify benchmark/evidence gaps after checking this state index. |
| P2 | `simd/dispatch-independence-stage1` | `proposals/simd_dispatch_independence_stage1_proposal.md` | Active Stage 1 cleanup for direct/runtime SIMD selection semantics. | Keep evidence focused on object shape and hot-call-site dispatch policy, not a larger ISA matrix. |
| P1 | `release/proof-repo-final-evidence` | `proposals/release_proof_repo_proposal.md` | Proposed release-blocking evidence packaging lane. | Implement scripts/templates now, but defer final proof capture until release-candidate source/API/docs/benchmark shape is frozen. |
| P1 | `work/composable-race` | `proposals/conflux_composable_race_full_proposal.md` | Active proposal, final design pass. | Implement only after root cancellation contracts and owner-local integration rules stay green under full tests. |
| DEFERRED | `http/streaming-upload-api` | `todo/server_gaps.md`, `docs/http-server-api.md` | Explicitly deferred future server API for bounded-memory request body and multipart upload streaming. | Current HTTP server intentionally buffers accepted request bodies in memory up to `max_body_size`; arbitrary large uploads need a separate streaming/spill-to-file surface with backpressure instead of raising the body cap. Do not start before the prerelease API/docs/evidence lanes settle. |

## Historical / implemented proposals

| Doc | Current state | Implementation guidance |
|---|---|---|
| `build/bench-regression-budget` | Implemented. | DB-backed per-benchmark budgets, `bench_budget_eval`, and `scripts/bench_check_budget.py` are the merge-gate shape; future work should tune budgets from host data, not rework the gate. |
| `docs/archive/proposals/README.md` | Implemented as a source-shape split inside the existing `conflux_file_io` target. | Leaf modules are `conflux.file_io.buffers`, `.pipe_pool`, `.reader`, `.iopoll`, and `.driver`; keep `conflux.file_io` as umbrella until consumers have migrated. |
| `docs/archive/proposals/README.md` | Implemented as private HTTP server module units. | `http_server_impl.cxx` is now facade glue over private state/recv/send/CQE/loop/dispatch/TLS/H2/WS units; treat further HTTP server work as targeted behavior/evidence, not another broad split. |
| `docs/archive/proposals/README.md` | Superseded by `.updated.md` and implemented target graph. | Do not follow old monolith/problem text as open work. Use package/component docs and `CMakeLists.txt`. |
| `docs/archive/proposals/README.md` | Implemented graph plus remaining coupling notes. | Historical target-boundary rationale. New work should be specific component polish, not another broad graph rewrite. |
| `docs/archive/proposals/README.md` | Superseded by `.updated.md` for source state. | Keep as rationale only; stream-vocabulary removal is complete. |
| `docs/archive/proposals/README.md` | Implemented, including diagnostic-print policy. | Direct reusable-source `std::print/std::println(stderr, ...)` is disallowed; the guard enforces source cleanliness while tests/examples/benchmarks keep human-facing `std::println`. |
| `docs/archive/proposals/README.md` | Implemented. | Keep for cancellation design rationale/tests. |
| `docs/archive/proposals/README.md` | Implemented. | Keep for recv-close generation rationale/tests. |
| `docs/archive/proposals/README.md` | Implemented. | Keep historical; async accept API is not open work. |
| `docs/archive/proposals/README.md` | Implemented for intended benchmark coverage. | Any remaining work is measurement/evidence, not missing API surface. |
| `docs/archive/proposals/README.md` | Implemented. | Follow-up only if HTTP send benchmarks prove more direct-format work is needed. |
| `docs/archive/proposals/README.md` | Implemented for plain/mapped path; TLS intentionally unchanged. | Remaining decision is threshold benchmarking, not SEND_ZC plumbing. |
| `proposals/t2_c_iopoll_ring_proposal.md` | Storage-only primitive implemented; HTTP/static adoption benchmark-gated. | Do not wire into HTTP static path until storage-read bottleneck is measured. |
| `proposals/conflux_package_component_visibility_v3_proposal.md` | Implemented package visibility v3. | Split exports, metadata-driven component import, closure-scoped external deps, support/requestable separation, and package visibility smokes landed. |
| `proposals/prerelease_2_day_coding_agent_plan.md` | Implemented by the prerelease readiness branch; active HEADER_INTERFACE, CONFLUX_ENABLE_DB, no_std_streams, and package-smoke checks moved to `docs/release-checklist.md`. | Treat as historical acceptance evidence. Follow-up cleanup starts from `conflux_prerelease_followup_cleanup_plan.md`. |
| `proposals/cancellable_task_authoring_plan.md` | Implemented. | Keep as historical rationale for root cancellation, cancellable task authoring, and race example ergonomics. |
| `proposals/http_deferred_task_cancellation_proposal.md` | Implemented. | Keep as historical rationale for bounded HTTP deferred handler task ownership and reasonful cancellation. |

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
