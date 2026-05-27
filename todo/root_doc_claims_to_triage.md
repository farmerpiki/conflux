# Root Document Claims to Triage

Status: open triage record
Date: 2026-05-21

This note replaces the removed root-level scratch docs:
`benchmarks.md`, `ergonomics_potential.md`, and
`conflux-feedback.json.md`.

Those files mixed historical feedback, branch-ranking notes, and now-landed
claims. Keep this file as a source-aware triage record only; do not treat it as
an active roadmap. Active work ordering belongs in `todo/proposal_state.md`,
`todo/parallel_priority_plan.md`, and module-specific docs/todos.

## Verification Summary

Checked against the current repository snapshot and
`../conflux_root_doc_triage.patch`:

- `scripts/check-component-map.py` exists and `scripts/check-component-map.py`
  reports `component-map: ok`.
- First-contact HTTP facade coverage exists through
  `tests/http_facade_import_smoke.cxx` and
  `tests/http_facade_api_snapshot.cxx`.
- JSON complaints from `conflux-feedback.json.md` are mostly satisfied by
  `docs/json-api.md`, `docs/json-cookbook.md`,
  `docs/json-boundary-guide.md`, `docs/json-reflect.md`, and
  `tests/json_test.cxx`.
- HTTP client limitations are intentionally documented in
  `docs/conflux-http-client-api.md`, but one doc/source drift remains around
  `HttpClientOptions::max_buffered_bytes`.
- Work cancellation/deadline/composition basics are documented and tested in
  `docs/conflux-work-root-api.md`, `docs/conflux-work-carrier-api.md`, and
  `tests/work_*`.

## Still Worth Tracking

### HTTP API Polish

- Keep auditing public docs and examples for stale unqualified `HttpRequest`,
  `HttpRequestView`, and `HttpResponse` first-contact examples. The
  quickstart/facade path already prefers `http::RequestView`, typed extractors
  such as `http::Path`, `http::Response` helpers, and `http::ClientRequest` /
  `http::ClientResponse` / `http::ClientResult`, but lower-level and advanced
  examples still use the legacy global spellings where they are intentionally
  closer to the raw router/server modules.
- Keep the complete HTTP umbrella, but keep narrow server/client imports visible
  in docs. `conflux.net.http.server` and `conflux.net.http.client` are better
  teaching surfaces than the aggregate umbrella when only one side is needed.
- Do not start a broad namespace cleanup from these notes. Any remaining work
  should stay limited to user-visible first-contact names and examples.

### JSON Remaining Questions

Most original JSON complaints are now covered. Keep only these as open review
questions:

- Interop with external JSON ecosystems may still need direct adapters beyond
  serialize/parse round-trips if real migrations keep paying that cost. Current
  provider-boundary helpers are the correct seam, but they do not by themselves
  provide direct adapters for every external DOM/event API.
- Framework-facing JSON should keep collapsing around typed HTTP helpers:
  `http::Json<T>`, typed response helpers, provider-explicit seams, and clear
  parse/decode error mapping. The raw DOM is already stronger; the main user
  experience risk is exposing too much DOM machinery on ordinary HTTP routes.
- Large-document traversal is no longer missing, but the desired golden path
  should remain explicit: choose `parse_view` / borrowed DOM, `JsonReader`,
  `JsonStreamReader`, `NdjsonRange`, or `JsonAccumulator` based on lifetime and
  streaming needs.

### HTTP Client Remaining Questions

- Streaming response consumption is still client-side incomplete for progressive
  rendering and client-side SSE. `ClientResponse::body` remains fully assembled;
  a callback, iterator, or body-chunk API would be the real missing surface.
- Timeout controls exist for core phases, but poll/read/write timeouts still map
  mostly through read/write errors with `os_errno == 0`. A distinct timeout
  classification remains useful.
- Connection pooling/reuse remains intentionally absent; docs say each call opens
  and closes a socket, and telemetry fields such as `pool_wait` /
  `reused_connection` are reserved for a later phase.
- Retry/backoff hooks, client-side SSE parsing, request body streaming, client
  HTTP/2/HTTP/3, content-coding decode, proxy client support, cookie jar, and
  client-side auth/tracing/retry interceptors remain real future surfaces.
- Testing/mocking injection is only partial. Resolver injection exists; transport
  or connection-factory injection is still worth considering before public v1 if
  client users need deterministic tests without rewriting call sites.

### Work Runtime Remaining Questions

- User-facing progress/event reporting for long-running jobs is still a product
  question. The source has task progress ownership/capability concepts, but that
  is not the same as a standard UI/TUI progress event channel.
- `when_all` and `race` exist in the carrier layer. `when_any` is not a current
  public surface, and `when_all_fast_fail` is documented as not yet delivering
  sibling-cancellation semantics because the carrier path is eager.
- Executor placement is clearer than the old notes suggested: `WorkPool`,
  `RingLane`, `async_run_on`, queue modes, and optional worker pinning exist.
  The remaining gap is application-facing guidance for UI/main-thread handoff.
- Queue instrumentation exists via `WorkPoolQueueStats` when enabled, but run
  time, wait time, and per-failure observability are still broader telemetry
  questions.
- Deterministic scheduling helpers/fake clocks are not a general public test
  surface. Timer tests currently use wall-clock waits and timerfd-driven helpers.
- High-level examples should keep improving the application-facing golden path,
  especially around handler shape, typed extractors, async middleware, and
  explicit offload.

## Covered In Other Docs

### Benchmarks

- Benchmark host hygiene, perf/correctness lane separation, DB-recorded benchmark
  runs, candidate comparison methodology, result interpretation, and the
  `--bench-info` / `--json` benchmark binary contract are covered by
  `benchmarks/README.md`, `benchmarks/reproducibility.md`, and
  `docs/project-policy.md`.

### Component Ownership

- Public component/package ownership and consumer-facing component names are
  covered by `docs/component-map.md`, `docs/package-consumption.md`, and
  `docs/public-api-map.md`. Automated drift checking already exists through
  `scripts/check-component-map.py` and passes on this snapshot.

### HTTP Server/App

- Server execution placement, borrowed/owned request lifetimes, sync-vs-async
  naming guidance, lifecycle/drain, pressure metrics, SSE, WebSocket, streaming
  responses, typed extractors, typed JSON helpers, route introspection,
  `app.validate()`, and response shortcut helpers are covered by current docs,
  examples, or facade snapshot tests.
- Compile coverage already proves server and client first-contact names can
  coexist in one translation unit.
- Non-throwing client request/config setup paths exist:
  `try_get` / `try_post` / `try_method` / `try_request` and
  `try_config_from_ini`.

### JSON

- Missing key vs explicit `null` is covered by `ObjectView::find_member`,
  `ObjectView::member`, and cookbook `optional_*` / `require_*` helpers.
- Object lookup cost is documented. Storage preserves member order and can warm
  a hash index explicitly or by parse-time `warm_threshold`.
- Deep extraction is covered by `JsonPath`, RFC 6901 JSON Pointer parsing, and
  `NodeRef::at_pointer`.
- Compact construction exists through `make_object` / `make_array`, with
  `ValueBuilder` / `ObjectBuilder` / `ArrayBuilder` remaining for incremental
  dynamic construction.
- Numeric access supports checked conversion through `JsonNumberView::to_i64()`,
  `to_u64()`, and `to_f64()`, and struct decode handles smaller
  signed/unsigned integral targets with range checks.
- Read-only DOM APIs do not expose a mutating `operator[]` path; lookup APIs are
  explicit and fallible.
- Pretty-print controls are documented through `JsonDumpOptions`.
- Borrowed vs owned input and arena/storage policy are visible through
  `parse_view`, `parse_borrowed`, `parse_copy`, `JsonDomPolicy`, and
  `JsonArena`.
- Streaming/incremental JSON exists through `JsonReader`, `JsonStreamReader`,
  `NdjsonRange`, and `JsonAccumulator`.
- Typed decode/reflection, duplicate-key policy, provider-boundary integration,
  and HTTP JSON provider seams are documented.

### HTTP Client

- Structured `HttpError`, phase-aware `HttpTimeouts`, blocking and async send,
  redirect limit handling, TLS verification diagnostics, telemetry, default
  headers, auth helpers, conditional request helpers, form bodies, and native
  JSON request helpers exist.
- The client docs explicitly state current Phase 1 limits: no pool/keep-alive,
  no HTTP/2 or HTTP/3 client, no content-coding decode, no request streaming,
  no proxy client, no cookie jar, and no retry/backoff.

### Work Runtime

- Root cancellation, cancellation reasons, scopes, deadlines, timers, carrier
  `when_all`, carrier `race`, queue modes, queue counters, and worker pinning are
  implemented or documented. The remaining work is ergonomics, telemetry breadth,
  and user-facing golden-path examples rather than the old blanket missing-core
  claims.
