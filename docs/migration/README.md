# conflux.work Migration Guide

Aggregated index of per-group migration guides for the conflux.work
ergonomy collapse (Phase 1, E1.0–E1.4).

---

## Per-group guides

| Group | File | Status |
|---|---|---|
| examples | [examples.md](examples.md) | done — no legacy usage |
| tests | [tests.md](tests.md) | done E1.2 — model_b section removed |
| net | [net.md](net.md) | done E1.3 — comment updated; UniqueFn pending E2a |
| file_io | [file_io.md](file_io.md) | done E1.3 — comment updated |
| db | [db.md](db.md) | done — no legacy usage |

---

## Migration timeline

| Step | Description | Status |
|---|---|---|
| E1.0 | Canonical `conflux::work::root::Task<T>` façade aliased as `conflux::work::Task<T>` | done |
| E1.1 | Old types become `[[deprecated]]` wrappers; compat test suite added | done |
| E1.2 | Per-group PRs migrate callsites | done |
| E1.3 | All callsites migrated; comment references updated; `baseline-work-v5-post-ergonomy` recorded | done |
| E1.4 | Wrappers deleted after one minor release cycle | pending |

---

## What changed and why

The three competing future types (`::Task<T>` Flow-backed, `root::Task<T>`
BasicResult-based, `model_a::Chain<T>`) are consolidated into one:

- **Canonical**: `conflux::work::root::Task<T>` (BasicResult-based)
- **Canonical carrier**: `conflux::work::carrier::model_a::Chain<T>`
- **Removed at E1.4**: `work_detail::Flow<T>`, `FlowSource<T>`, `task_as_flow`,
  `model_b::*`, pipe operator `|`, step types

See the full rationale in `api_traps.md` traps 1.20 (fragmentation), 1.7
(async fragmentation), and the attack plan §1 (Status quo).
