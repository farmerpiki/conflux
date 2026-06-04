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
- [ ] Content-Length with `Transfer-Encoding` request smuggling defense returns
  `400` and increments `content_length_with_transfer_encoding`.
- [ ] Aggregate header bytes/count limits return `431` with
  `header_block_too_large`.
- [ ] Body limits return `413` with `body_too_large`.
- [ ] Header timeout / slowloris and incomplete-body timeout behavior are
  covered by the runtime timeout policy for this deployment.
- [ ] Static file mounts reject path traversal and symlink policy is intentional.
- [ ] Trusted proxy headers are enabled only for known proxy source addresses.
- [ ] Structured logs redact `Authorization`, `Proxy-Authorization`, `Cookie`,
  `Set-Cookie`, `X-Api-Key`, `X-Api-Token`, `X-Auth-Token`, and `X-CSRF-Token`.

## Lifecycle

- Install a shutdown path that calls `conflux::http::HttpServer::drain()` or `shutdown()`.
- For controller-thread startup, call `port()` after `run()` starts to wait for
  listen readiness; run `App::validate()`, `try_server()`, or `try_run()` before
  depending on readiness.
- Pick a drain deadline that matches the service's load balancer and process
  supervisor timeout.
- Decide whether SSE/WebSocket streams should finish or close during drain.
- Tie application background tasks and certificate reload controllers to the
  same cancellation/supervision path as the HTTP server drain.
- Capture `build_info_summary()`, `conflux::http::HttpServer::startup_report()`, and the
  redacted `Config` summary in startup logs when the service owner wants
  diagnostics.
- Install `http::observability()` for request IDs, trace context, structured
  access logs, and route metrics. Export `conflux::http::HttpServer::metrics().pressure` and
  application-owned `SseChannel::pressure_metrics()` through explicit
  observability sinks when server pressure needs to be scraped too.

## Pressure

| Boundary | Owner | Policy vocabulary | Required decision/evidence |
|---|---|---|---|
| Accept/admission | HTTP server | `OverflowPolicy::reject` | Capacity, late-accept close behavior, and `pressure.accept_rejected`. |
| Request body | HTTP server | parser rejection | `max_body_size` and `body_too_large` rejection path. |
| Response send | HTTP server | `OverflowPolicy::backpressure` | Slow-client timeout/drain deadline and `response_backpressure_events` / `drain_forced_close`. |
| SSE channel | Application channel | `drop_newest`, `drop_oldest`, `close_connection` through `SseOverflowPolicy` | Queue capacity, overflow policy, and `SseChannel::pressure_metrics()`. |
| WebSocket outbound | Application after `WsConn` handoff | chosen `OverflowPolicy` for any app broadcast queue | Queue capacity and close/backpressure policy; server-owned handoff pressure uses `websocket_closed_for_pressure`. |
| Worker/offload pool | Application executor | bounded queue reject/backpressure through `WorkPoolOptions` | `max_inject_queue`, `local_queue_capacity`, `enqueue(job)` rejection behavior, and `work_pool_rejected_total` / `work_pool_queue_depth` when exported. |
| DB pool, when enabled | Application DB component | bounded wait through `PoolConfig::acquire_timeout` | `min_connections`, `max_connections`, acquire timeout, queued-acquire cancellation, and timeout/retry policy. |
| Ring CQ overflow | HTTP server/runtime | fatal overflow reporting | Ring sizing, fatal-overflow handling, and `cq_overflow` reporting. |

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
