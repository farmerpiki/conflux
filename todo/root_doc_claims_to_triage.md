# Root Document Claims to Triage

Date: 2026-05-21

This file preserves only claims from removed root-level notes that still appear
worth discussing after a source/doc spot-check. The original files were stale as
active docs:
`benchmarks.md`, `ergonomics_potential.md`, and `conflux-feedback.json.md`.

Claims that were already satisfied by the current tree are kept at the end with
evidence so they do not keep resurfacing as open work.

## Benchmarking Claims

- Benchmark runs need a quiet machine: performance governor, no swap pressure, no
  concurrent heavy work, and no interactive diagnosis while a benchmark is
  executing. Use timeouts for hard stops and inspect hangs after the run.
- Correctness/sanitizer lanes and benchmark/performance lanes should remain
  separate. Benchmark artifacts are built from `perf-*` or explicit release
  lanes, not sanitizer/debug correctness builds.
- Benchmark evidence should prefer DB-recorded runs through
  `scripts/bench_record.sh` when comparing candidates, because the recorder
  captures descriptors, raw NDJSON, summaries, manifests, and preset metadata.
- Cross-candidate performance comparisons should be interleaved after all
  candidates are prebuilt. Order effects should be checked from the recorded
  `round`/`position` metadata before accepting small deltas.
- Allocation-dominated microbenchmarks may be judged by best/p10, while
  throughput workloads usually need median/p10 and noise checks. High MAD on a
  CPU-bound run is environmental evidence, not product evidence.
- New recordable benchmark binaries should implement `--bench-info` and
  `--json`, then be added to the recorder target list in
  `benchmarks/CMakeLists.txt`.
- Benchmarks that create threads or perform I/O per measured invocation should
  avoid hidden internal repetition that changes the measured workload.

## HTTP API Claims

- First-contact server docs and examples should prefer the server-first
  namespace/import path and avoid pulling in the broad HTTP umbrella when the
  example is server-only.
- The complete HTTP umbrella remains useful, but docs should distinguish it from
  narrower server/client imports so examples do not accidentally teach unrelated
  client APIs.
- Unqualified `HttpRequest`, `HttpRequestView`, and `HttpResponse` examples are
  still worth auditing before v1. The final first-contact shape should favor
  `http::RequestView`, `http::OwnedRequest`/`http::Request`,
  `http::Response`, `http::ClientRequest`, `http::ClientResponse`, and
  `http::ClientResult`.
- Compile coverage should keep proving that server and client first-contact
  names can coexist in one translation unit.
- Do not start a broad namespace cleanup from these notes. Any remaining work
  should be limited to user-visible first-contact names and examples.

## Component Map Claim

- `docs/component-map.md` asks maintainers to keep component entries synced with
  `conflux_public_component(...)`. A small drift check may still be useful if no
  current script validates that mapping directly.

## JSON Claims

- Read APIs should make missing keys, explicit JSON null, and wrong-type values
  easy to distinguish without repetitive boilerplate.
- Object lookup behavior and cost should remain documented. If object storage is
  still vector-backed, repeated lookup cost may matter for hot payload paths.
- Path access helpers may still be useful for deep extraction if current APIs
  require repeated manual null/type checks.
- Protocol-payload construction may still need a compact object/array builder or
  literal-like helper if current `set(...)` usage remains noisy.
- Numeric access should support checked coercion into caller-requested numeric
  types without repeated manual `int64_t`/`uint64_t`/`double` probing.
- Interop with other JSON ecosystems may need adapters beyond serialize/parse
  round-trips if migrations keep hitting this cost.
- Const/read APIs should steer callers away from accidental mutation patterns,
  especially around `operator[]`.
- Pretty-print controls should be deterministic enough for logs and debugging.
- Large-document traversal may need an explicit borrow/view-oriented path if
  repeated reads are important.
- JSON best practices around borrowed vs owned data, missing/null behavior, and
  decode policy should remain visible from docs and signatures.

## HTTP Client Claims

- Streaming response consumption still appears client-side incomplete:
  `ClientResponse::body` is documented as fully assembled, and
  `HttpClientOptions::max_buffered_bytes` is documented as unused. A callback,
  iterator, or body-chunk API may still matter for progressive rendering and
  client-side SSE.
- Timeout controls exist for core phases, but `docs/conflux-http-client-api.md`
  says poll timeouts currently surface as read/write errors with
  `os_errno == 0`; a distinct timeout error classification may still be useful.
- Retry policy hooks may still be useful if callers keep reimplementing retries.
- Connection pooling/reuse should be visible at the API level if the client is
  intended for typical API workloads. The client docs still say Phase 1 opens
  and closes a socket per call.
- Client-side SSE ergonomics may need a dedicated parser or iterator. Server SSE
  exists; this claim is specifically about consuming SSE as an HTTP client.
- Backpressure and maximum buffering controls should be explicit for streaming
  response bodies; the current client doc marks `max_buffered_bytes` unused.
- Request body streaming may be needed for large upload flows. The client docs
  say no `Transfer-Encoding: chunked` request bodies are sent.
- HTTP protocol selection/diagnostics for client h1/h2/h3 should be clear to
  callers.
- Redirect, cookie, proxy, compression, and TLS diagnostics should remain
  explicit policy surfaces instead of hidden behavior.
- Client-side interceptors for auth refresh, tracing, retries, and metrics may
  still be useful.
- Testing and mocking injection points should be easy enough that callers do not
  need to rewrite call sites.

## Work Runtime Claims

- Long-running jobs may need a standard progress/event channel for TUI/GUI and
  operational integrations.
- Composition utilities such as `when_any`, `when_all`, race, and timeout
  wrappers may still be useful. `when_all` appears in migration docs; confirm
  whether the complete user-facing set exists before treating this as open.
- Executor or thread-affinity selection should be clear when integrating with UI
  or main-thread callback handoff.
- Instrumentation hooks for queue depth, run time, wait time, and failures may
  still be needed.
- Deterministic scheduling helpers or fake clocks may be useful for async tests.
- High-level examples should make the intended application-facing golden path
  easier to discover.

## Landed or Mostly Landed Claims

- Client/server request and response first-contact names largely exist:
  `ClientRequest`, `ClientResponse`, `ClientResult`, `RequestView`,
  `OwnedRequest`/`Request`, and `Response`.
- Non-throwing client request and config setup paths exist:
  `try_get`/`try_post`/`try_method`/`try_request` and `try_config_from_ini`.
- Context route sugar exists on the router for at least `get_context`, and docs
  describe context-specific registration.
- The template namespace has a canonical `conflux::templates` spelling in tests.
- Typed HTTP request field accessors are documented on `RequestView` and
  `OwnedRequest`, and `tests/http_core_test.cxx` covers typed query, form,
  header, cookie, optional, and missing/invalid errors.
- JSON typed decode/reflection and provider-boundary helpers exist, so the old
  blanket "no typed decode layer" claim is obsolete.
- JSON duplicate-key policy is documented and controllable through
  `DuplicateKeyPolicy`.
- JSON incremental/streaming support exists through `JsonStreamReader`,
  `NdjsonRange`, and `JsonAccumulator` in `docs/json-api.md`.
- Server SSE and streaming/chunked response support exist. `docs/http-server-api.md`
  documents `SseChannel`, bounded overflow policies, drain behavior, and
  streaming responses; tests cover `SseChannel` overflow policies.
- HTTP client errors now use structured `HttpError` rather than
  `expected<..., string>` as the primary documented shape.
- Async HTTP client support exists through `async_send`; the old
  "synchronous-only primary path" claim is obsolete.
- HTTP client timeout knobs exist through `HttpTimeouts`, including connect and
  first-byte coverage in tests.
- HTTP client telemetry exists for peer address, connect, TLS, TTFB, body, and
  byte counts, though partial telemetry is not returned on failure.
- HTTP client header/auth helpers exist: `.bearer`, `.basic`, `.accept_json`,
  `.content_type`, conditional request helpers, form bodies, and default headers.
- Work cancellation, cancellation reasons, scopes, deadlines, and deadline
  timers exist in `conflux.work`; the old blanket "no first-class cancellation"
  and "no deadline primitives" claims are obsolete.
- HTTP server shutdown/drain behavior is documented and tested, including
  pressure metrics and configurable drain options.
