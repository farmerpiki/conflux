# Migration Guide — net/

Tracks callsite migration for `src/net/` as part of E1.2.

## Inventory

### `src/net/http_server.cxx` — `work_detail::UniqueFn` (2 occurrences)

| Line | Before | After |
|------|--------|-------|
| 730 | `deque<work_detail::UniqueFn<void()>> pending_ops{};` | `deque<std::move_only_function<void()>> pending_ops{};` |
| 1478 | `work_detail::UniqueFn<void()> op)` | `std::move_only_function<void()> op)` |

Lands in E2a PR for the `net` group.

### `src/net/dns/dns.cxx` — comment (1 occurrence)

| Line | Before | After |
|------|--------|-------|
| 1785 | Comment mentioning `FlowSource` | Update wording to `TaskSource` |

Lands in E1.3 comment-cleanup pass.

## Before / After pairs

_Populated as E1.2 PRs land._
