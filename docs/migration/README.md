# conflux.work Migration Guide

Aggregated index of per-group migration guides for the conflux.work
ergonomy collapse (Phase 1, E1.0–E1.4).

---

## Per-group guides

| Group | File | Status |
|---|---|---|
| examples | [examples.md](examples.md) | done — no legacy usage |
| tests | [tests.md](tests.md) | done E1.2 — model_b section removed |
| net | [net.md](net.md) | done — comment and UniqueFn cleanup complete |
| file_io | [file_io.md](file_io.md) | done — comment cleanup complete |
| db | [db.md](db.md) | done — no legacy usage |

---

## Migration timeline

| Step | Description | Status |
|---|---|---|
| E1.0 | Canonical `conflux::work::root::Task<T>` façade aliased as `conflux::work::Task<T>` | done |
| E1.1 | Old types become `[[deprecated]]` wrappers; compat test suite added | done |
| E1.2 | Per-group PRs migrate callsites | done |
| E1.3 | All callsites migrated; comment references updated; `baseline-work-v5-post-ergonomy` recorded | done |
| E1.4 | Wrappers deleted after migration | done |

---

## What changed and why

The formerly competing async/carrier shapes (`::Task<T>` Flow-backed,
`root::Task<T>` BasicResult-based, historical `model_a::Chain<T>`, and
`model_b::*`) are consolidated into the current public spellings:

- **Canonical root value**: `conflux::work::root::Task<T>`
- **Canonical carrier**: `conflux::work::carrier::Chain<T>`
- **Removed from current source**: `work_detail::Flow<T>`, `FlowSource<T>`,
  `task_as_flow`, `conflux::work::carrier::model_a`,
  `conflux::work::carrier::model_b`, pipe operator `|`, step types

Repository-owned rationale now lives in [`../execution-model.md`](../execution-model.md),
[`../concurrency-naming-model.md`](../concurrency-naming-model.md), and
[`../pre-v1-migration-contract.md`](../pre-v1-migration-contract.md). Historical
external notes about API fragmentation should be treated as background, not as an
in-repo contract.
