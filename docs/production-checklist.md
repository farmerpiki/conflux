# Production checklist

Use this as a deployment-facing checklist for HTTP services built on conflux.
`examples/advanced/production_showcase.cxx` shows these checks wired together in
one small service.

## Lifecycle

- Install a shutdown path that calls `HttpServer::drain()` or `shutdown()`.
- Pick a drain deadline that matches the service's load balancer and process
  supervisor timeout.
- Decide whether SSE/WebSocket streams should finish or close during drain.
- Capture `build_info_summary()`, `HttpServer::startup_report()`, and the
  redacted `Config` summary in startup logs when the service owner wants
  diagnostics.
- Export `HttpServer::metrics().pressure` and application-owned
  `SseChannel::pressure_metrics()`.

## Pressure

| Boundary | Required decision |
|---|---|
| Accept/admission | Capacity, rejection/close behavior, metric. |
| Request body | Maximum size and rejection path. |
| Response send | Backpressure behavior and slow-client timeout. |
| SSE channel | Queue capacity, overflow policy, per-channel metric. |
| WebSocket outbound | Queue capacity and close/backpressure policy. |
| Worker/offload pool | Queue capacity and reject/backpressure behavior. |

## Evidence

- Run examples and quickstart checks before release.
- Run HTTP e2e tests in the same build mode used for release evidence.
- Benchmark release builds only; do not use debug or sanitizer builds for
  performance claims.
