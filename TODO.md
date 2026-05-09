# Open Work — verified 2026-05-09

Items verified against source. PARTIAL = code exists but incomplete. NOT DONE = absent.

---

## ATTACK_PLAN.md

| # | Item | State |
|---|------|-------|
| Task 1 | `format_response` close flag — H2/deferred error path now passes `close=true`; SSE headers now respect `conn.close_after_send` | DONE |
| Task 4a | `resolve_timeouts()` in client.cxx defined but never called — dead code; per-request timeouts not merged with client defaults | DONE |
| Task 7 | proxy.cxx still calls `client.send_blocking`; Phase 2 async migration not started | NOT DONE |
| Task 8 | `DeferredResponse` has no `TaskControl` ref; timeout → 504 but underlying task keeps running | NOT DONE |
| Task 9 | `Op::RingLaneWake` absent from server event loop; `RingLane` not instantiated in http_server.cxx | NOT DONE |
| Task 10 | `RequestContext` zero hits codebase-wide; handler shape unchanged | NOT DONE |
| Task 12 | `conn.partial.erase(0,n)` memmove still at 3 sites; no `{buffer, read_pos}` struct | NOT DONE |

---

## JSON_PLAN.md

| Item | State |
|------|-------|
| Phase 2.3 — `PathFrame` uses `V<PathFrame>` (heap); plan required `SmallVector<PathFrame,16>` (inline-16, zero heap at depth ≤ 16) | PARTIAL |
| Phase 4.3 — `JsonCodec<Document>::decode(JsonReader&)` specialization absent | NOT DONE |
| Phase 4.4 — corpus files exist but not loaded in `json_bench.cxx`; alloc/op, p50/p95 latency, DOM-vs-pull comparison not benchmarked | PARTIAL |
| Phase 5.2 — `JsonArenaOptions::intern_keys` field absent; `slab_used()` stub returns 0 | PARTIAL |
| Phase 8.2 — compile-time JSON literal parsing (`parse_ct`) not started (plan deferred: "design not finalized") | NOT DONE |
| Infra — JSONTestSuite conformance gate absent; benchmark harness missing alloc/op + latency metrics | NOT DONE |

---

## docs/CONFLUX_ASYNC_DNS_PROPOSAL.md

| Item | State |
|------|-------|
| RFC 8305 §4 connect-attempt staggering (250 ms delay between attempts) — absent from client.cxx and client_async.cxx; **Implementation Status section incorrectly claims this is done** | NOT DONE |
| client.cxx bare `::getaddrinfo` fallback at :249 retained when no resolver supplied; proposal says "old getaddrinfo paths deleted" | PARTIAL |

---

## docs/db-remaining.md

| Item | State |
|------|-------|
| P7 — true libpq wire pipeline (`PQenterPipelineMode` / `PQpipelineSync` / `PQexitPipelineMode`); current impl is logical batching barrier only | NOT DONE |
| P7 — live Postgres CI run with `PG_TEST_CONNINFO`; tests currently guarded by SKIP | NOT DONE |
| P7 — stronger Pipeline teardown (blocking destructor, true pipeline close) | NOT DONE |
| P7 — ~~stale "Still open" bullet: `db_pipeline_bench.cxx` is already present at `benchmarks/db_pipeline_bench.cxx` — bullet should be struck~~ | DONE |
| P6 — `Connection::copy_in` / `copy_out` (deferred until consumer appears) | NOT DONE |
| P4 — single-row streaming (`query_stream` / `PQsetSingleRowMode`); blocked on `WorkStream<T>` framework primitive | NOT DONE |
| P14 — namespace rename `conflux::db` → `conflux::pg` (decision pending) | NOT DONE |
| Bench — `db_coro_bench --binary` (binary format decode path) not benchmarked | NOT DONE |

---

## docs/work_migration_inventory.md

| Item | State |
|------|-------|
| Status table row "net (http_server) — pending E2a — UniqueFn not an E1.x type" is stale; `UniqueFn` already replaced with `small_move_only_function` in http_server.cxx | DONE |

---

## docs/conflux-work-root-api.md + docs/conflux-work-carrier-api.md

| Item | State |
|------|-------|
| Both docs use `JoinContextError` / `JoinContextReason` throughout; actual class in root.cxx is `JoinError` with inner `reason` enum — **docs are wrong** | DONE |

---

## docs/pre-v1-work-http-plan.md

| Item | State |
|------|-------|
| `http::Task<Response>` alias — plan describes it as preferred easy-layer shape; not exported from `conflux.net.http` or `conflux.net.app` | NOT DONE |
| DNS and DB internals not yet migrated to `root::Task<T>` (acknowledged active workstream) | PARTIAL |

---

## work_attack_plan.md (../work_attack_plan.md)

| Item | State |
|------|-------|
| E1.z — `into_task_unchecked()` still present; v7 B4 said to drop it | DONE |
| E2b.1 — `try_set_cancelled` public setter still takes internal `CancelReason` enum, not `work_errc` as required | DONE |
| E4 — `try_set_error(ec, string_view message) noexcept` overload missing from `BasicSource<T>` | DONE |
| E4 — `concept work_handle<H>` simplified; missing `awaits_outcome<>` requires-clause | PARTIAL |
| E4 — `BasicResult` / `BasicJoinHandle` collapse not done (deferred; `// [REVISIT]` comment at root.cxx:1984) | NOT DONE |
| P2b — Fix B (`alignas(64)` on Cold) used without a code comment explaining false-sharing rationale; plan mandated the comment on `hot_pad` member (which doesn't exist) | DONE |
| P9 — `join_all` only stores `first_error`; `AggregateError::causes_view()` for multiple-failure observability absent | PARTIAL |
