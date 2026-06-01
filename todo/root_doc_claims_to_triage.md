# Root Document Claims To Triage

Status: open triage TODO

This file tracks only root-document follow-up questions that are still useful.
Resolved claims were removed instead of kept as a historical checklist.

## HTTP API Polish

- [ ] Keep auditing public docs and examples for stale unqualified
  `HttpRequest`, `HttpRequestView`, and `HttpResponse` in first-contact examples.
  Advanced/raw-router docs may keep legacy spellings when they are intentionally
  documenting the lower-level surface.
- [ ] Keep narrow server/client imports visible in docs where they teach better
  than the aggregate `conflux.net.http` umbrella.

## JSON Questions

- [ ] Add direct adapters for external JSON DOM/event ecosystems only when real
  migrations show parse/serialize round trips are still a cost.
- [ ] Keep ordinary HTTP-route docs centered on typed helpers such as
  `http::Json<T>`, typed response helpers, provider-explicit seams, and clear
  parse/decode error mapping.
- [ ] Keep large-document guidance explicit about choosing `parse_view`,
  `parse_borrowed`, `parse_copy`, `JsonReader`, `JsonStreamReader`,
  `NdjsonRange`, or `JsonAccumulator` based on lifetime and streaming needs.

## HTTP Client Questions

- [ ] Add progressive streaming response consumption for rendering and
  client-side SSE.
- [ ] Distinguish poll/read/write timeout classification from generic
  read/write errors with `os_errno == 0`.
- [ ] Add connection pooling/reuse only as a later explicit phase.
- [ ] Track retry/backoff hooks, client-side SSE parsing, request body
  streaming, client HTTP/2/HTTP/3, content-coding decode, proxy support, cookie
  jar, and client-side auth/tracing/retry interceptors as future surfaces.
- [ ] Consider transport or connection-factory injection before v1 if client
  users need deterministic tests without rewriting call sites.

## Work Runtime Questions

- [ ] Decide whether long-running jobs need a standard user-facing
  progress/event channel.
- [ ] Decide whether `when_any` or sibling-cancelling `when_all_fast_fail` should
  become public surfaces.
- [ ] Improve application-facing guidance for UI/main-thread handoff.
- [ ] Expand telemetry beyond queue counters if run time, wait time, and
  per-failure observability become release requirements.
- [ ] Consider deterministic scheduling helpers or fake clocks as a public test
  surface.
- [ ] Keep improving golden-path examples around handler shape, typed
  extractors, async middleware, and explicit offload.
