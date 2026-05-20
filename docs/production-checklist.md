# Production Checklist

Use this as a deployment-facing checklist for HTTP services built on conflux.
`examples/advanced/production_showcase.cxx` shows these checks wired together in
one small service.

## Preset Review

- [ ] `development` is used only for local work; limits and timeouts are still
  bounded.
- [ ] `public_server` is the default internet-facing preset; strict config,
  parser limits, body limits, timeouts, and smuggling defenses are enabled.
- [ ] `low_latency` is selected only with explicit owner sign-off and still has
  bounded size/time limits.
- [ ] `benchmark` is not used for production traffic.
- [ ] Any `unsafe_max_speed`-style preset is treated as unsafe and must not be
  deployed publicly.

## Request Admission

- [ ] Duplicate `Content-Length` returns `400` and increments
  `duplicate_content_length`.
- [ ] `Content-Length` with `Transfer-Encoding` returns `400` and increments
  `content_length_with_transfer_encoding`.
- [ ] Aggregate header bytes/count limits return `431` with
  `header_block_too_large`.
- [ ] Body limits return `413` with `body_too_large`.
- [ ] Header timeout / slowloris behavior is covered by the runtime timeout
  policy for this deployment.
- [ ] Static file mounts reject path traversal and symlink policy is intentional.
- [ ] Trusted proxy headers are enabled only for known proxy source addresses.
- [ ] Structured logs redact `Authorization`, `Proxy-Authorization`, `Cookie`,
  `Set-Cookie`, `X-Api-Key`, `X-Api-Token`, `X-Auth-Token`, and `X-CSRF-Token`.

## Lifecycle

- Install a shutdown path that calls `HttpServer::drain()` or `shutdown()`.
- Pick a drain deadline that matches the service's load balancer and process
  supervisor timeout.
- Decide whether SSE/WebSocket streams should finish or close during drain.
- Capture `build_info_summary()`, `HttpServer::startup_report()`, and the
  redacted `Config` summary in startup logs when the service owner wants
  diagnostics.
- Install `http::observability()` for request IDs, trace context, structured
  access logs, and route metrics. Export `HttpServer::metrics().pressure` and
  application-owned `SseChannel::pressure_metrics()` through explicit
  observability sinks when server pressure needs to be scraped too.

## Pressure

| Boundary | Required decision |
|---|---|
| Accept/admission | Capacity, rejection/close behavior, metric. |
| Request body | Maximum size and rejection path. |
| Response send | Backpressure behavior and slow-client timeout. |
| SSE channel | Queue capacity, overflow policy, per-channel metric. |
| WebSocket outbound | Queue capacity and close/backpressure policy. |
| Worker/offload pool | Queue capacity and reject/backpressure behavior. |

## Observability

- Keep metric labels low-cardinality: route pattern, method, service, status,
  and status class only.
- Do not log query strings unless the service has reviewed them for secrets.
- Keep header logging off by default. If enabled, keep sensitive-header
  redaction on and add application-specific secret headers.
- Protect `/metrics` with a network boundary or app middleware when exposed
  outside a trusted listener.

## Evidence

- Run examples and quickstart checks before release.
- Run HTTP e2e tests in the same build mode used for release evidence.
- Benchmark release builds only; do not use debug or sanitizer builds for
  performance claims.
