# Migration Guide — net/

Tracks completed callsite migration for `src/net/` as part of E1.2/E1.3/E2a.

## Inventory

No pending `src/net/` migration inventory remains. Historical cleanup completed:

- `src/net/http_server.cxx` no longer uses `work_detail::UniqueFn`; the remaining
  move-only callbacks use the standard move-only function surface.
- `src/net/dns/dns.cxx` no longer carries the old `FlowSource` wording.

## Before / After pairs

_Populated as E1.2 PRs land._
