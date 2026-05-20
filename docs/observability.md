# Observability

Conflux HTTP apps can install the unified observability facade:

```cpp
auto app = conflux::http::app();
app.use(conflux::http::observability({
    .service_name = "api",
    .metrics_path = "/metrics",
}));
```

Defaults:

- request ID header propagation with `X-Request-ID`
- W3C `traceparent` propagation
- one structured JSON access log event per request
- Prometheus text metrics at `/metrics`
- route-pattern labels, with `<unmatched>` for 404s
- sensitive-header redaction

Redacted headers are `Authorization`, `Proxy-Authorization`, `Cookie`,
`Set-Cookie`, `X-Api-Key`, `X-Api-Token`, `X-Auth-Token`, and `X-CSRF-Token`.
Add service-specific names with `extra_sensitive_headers`.

Metrics intentionally use low-cardinality labels:

```text
http_requests_total{service,route,method,status_class,status}
http_request_duration_seconds_bucket{service,route,method,le}
http_request_duration_seconds_sum{service,route,method}
http_request_duration_seconds_count{service,route,method}
http_rejections_total{service,reason,status}
```

Query strings are never metric labels and are excluded from access logs by
default. Header logging is also off by default; if enabled, redaction still
applies.

The facade does not add an exporter or global singleton. Existing lower-level
middleware such as `request_id_middleware`, `tracing_middleware`,
`structured_log_middleware`, and `metrics_middleware` remains available for
services that need custom wiring.

When an `App` with observability creates a `HttpServer`, it passes internal
server hooks that record parser/admission rejections into the same registry as
the app middleware. Manually constructed `Router` + `HttpServer` stacks can use
`observability_server_hooks()` if they want the same unified registry.

Server pressure, work-pool, task-allocation, and JSON-arena metrics are only
emitted when the app supplies an explicit source:

```cpp
auto pool = std::make_shared<WorkPool>();
app.use(conflux::http::observability(
    {
        .work_pools = {{"default", pool}},
        .task_allocation_metrics = true,
    },
    {
        .pressure_metrics = [&server] { return server.metrics().pressure; },
    }));
```

Streaming responses are measured at response creation/header commit time. Export
stream-close counters from the stream owner when tail duration is needed.
