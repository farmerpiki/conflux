# Root Document Claims To Triage

Status: open triage TODO

This file tracks only root-document follow-up questions that are still useful.
Resolved claims were removed instead of kept as a historical checklist.

## JSON Questions

- Deferred: add direct adapters for external JSON DOM/event ecosystems only when real
  migrations show parse/serialize round trips are still a cost.

## HTTP Client Questions

- Later explicit phase: add client connection pooling/reuse only after the
  current client contract settles. Server-side proxy/auth/cookie/tracing modules
  exist; this item is about the client transport still opening one connection
  per request.
- Future client surfaces still absent today: retry/backoff policy hooks,
  client-side SSE parsing, request body streaming/chunked send, client HTTP/2
  or HTTP/3, content-coding decode, client-side proxy support, cookie jar, and
  client-side auth/tracing/retry interceptors.
- Future decision: consider transport or connection-factory injection before v1
  if client users need deterministic tests without rewriting call sites.

## Work Runtime Questions

- Conditional future telemetry: expand beyond queue counters if run time,
  wait time, and per-failure observability become release requirements.
- Future test surface: consider deterministic scheduling helpers or fake clocks
  as a public test surface.
