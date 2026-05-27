# Archived Proposals

Archived proposal summaries are historical rationale, not branch-selection
inputs. The source proposal files are not shipped in this archive directory. Use
`todo/proposal_state.md` and `todo/parallel_priority_plan.md` for current work
ordering.

## 2026-05

| Work area | Outcome |
|---|---|
| Stream policy | Superseded by the updated stream policy and implemented; reusable source stderr checks are guarded by script/CTest. |
| File I/O module split | Implemented as file I/O leaf module units inside the existing target. |
| HTTP server implementation split | Implemented as private HTTP server implementation units. |
| JSON module split | Implemented for parser/storage/source-shape cleanup. |
| Modular build targets | Superseded by the updated target graph proposal and implemented as the component/package target graph. |
| HTTPS async cancellation | Implemented HTTPS async cancellation/connect timeout work. |
| Cancel by fd / recv shutdown | Implemented cancel-by-fd/recv shutdown work. |
| Socket task ring benchmark | Implemented intended socket task ring benchmark coverage. |
| TCP async accept | Implemented async accept API and tests. |
| Registered send buffers | Implemented registered send-buffer path. |
| SEND_ZC | Implemented plain/mapped SEND_ZC plumbing; threshold work is evidence-only. |
| Template compiled cache | Implemented compiled template cache/reload work. |
