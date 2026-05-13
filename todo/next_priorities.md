# Next Implementation Priorities

This list is intentionally ordered so future patch work can pick the first open
item instead of re-deciding from scratch. It is based on the current `todo/`,
`proposals/`, and top-level design documents in this source snapshot.

## Selection rules

1. Prefer contract-breaking modularity and API-boundary fixes before deeper perf
   work; this is still pre-v1, so API breakage is acceptable when it improves
   ergonomics or performance.
2. Prefer coherent target-split and dependency-boundary chunks over tiny one-target patches, while still avoiding large rewrites.
3. Only implement benchmark-gated performance changes when the benchmark harness
   or measurement target already exists.
4. Defer speculative architecture rewrites until allocation/benchmark data proves
   the current path is the bottleneck.

## Ordered list

1. **Finish modular CMake target slices.**
   - Current status: `core`, `json`, `file_io_sync`, `file_map`, `file_io`,
     `socket_io`, `utils`, `net_config`, `net_cancel`, `net_tls`, `http_core`,
     `http_json`, `http_router`, `http_policy`, `http_auth`,
     `http_observability`, `http_openapi`, `http_vhost`, `http_client`,
     `http_async_client`, `http_compression`, `http_proxy`, `http1`, `http2`,
     `http3`, `smtp`, `template`, and `template_watch` have started splitting out.
   - Completed slice: `conflux::json_file` / `conflux.json.file` is now separate
     and depends only on `json + file_io_sync`.
   - Completed prerequisite slice: `conflux::utils` and `conflux::net_config`
     are separate module targets instead of being compiled into the HTTP
     monolith.
   - Completed prerequisite slice: `conflux::net_cancel` and `conflux::net_tls`
     are separate targets; router/server still import TLS, but no longer need TLS
     source compiled inside the monolith.
   - Completed larger HTTP middleware slice: `conflux::http_router`,
     `conflux::http_policy`, `conflux::http_auth`,
     `conflux::http_observability`, `conflux::http_openapi`, and
     `conflux::http_vhost` are separate module targets; the monolith links them
     instead of compiling router/middleware modules directly.
   - Completed larger HTTP client/compression/proxy slice: `conflux::http_client`,
     `conflux::http_async_client`, `conflux::http_compression`,
     `conflux::http_proxy`, `conflux::smtp`, and internal `conflux::_dns_bridge`
     are separate module targets; the monolith links them instead of compiling
     their module/interface/private implementation units directly.
   - Completed larger HTTP protocol/server/app/umbrella slice: `conflux::process`,
     `conflux::net_io_buffer`, `conflux::http_protocol`, `conflux::http_server`,
     `conflux::http_app`, and `conflux::http` are separate module targets; the
     legacy aggregate `conflux` target now compiles only its top-level umbrella
     module and links the component graph.
   - Completed optional protocol split: `conflux::http1`, `conflux::http2`, and
     `conflux::http3` now sit under the thin `conflux::http_protocol` umbrella.
   - Completed static/realtime surface slice: `conflux::http_static` owns the
     exported `StaticOptions` module surface, `conflux::http_core` owns the
     server request/view/callback vocabulary, and `conflux::http_realtime` owns
     the exported SSE plus WebSocket surfaces. `conflux.net.router` re-exports
     those modules for compatibility.
   - Completed static implementation split slice: `conflux::http_response`,
     `conflux::http_static_core`, and `conflux::http_static_async` now own the
     response vocabulary, static request/cache/path helpers, static root-dir
     ownership, contained open/probe helpers, GET/PUT/DELETE execution paths, and
     async static file helper coroutines. Router keeps route registration only.
   - Completed stale-edge cleanup slice: `conflux::http_router` no longer imports
     or links `conflux.file_io` only to serve static PUT/DELETE internals; those
     live behind `conflux::http_static_async`.
   - Completed dependency-edge cleanup slice: rechecked stale proposal edges for
     `conflux_socket_io -> file_io`, `net.client -> file_io`, and
     `HttpRequest::Builder::body_json(NodeRef)`; those are already gone. Also
     removed redundant direct `PkgConfig::LIBURING` links from higher-level
     component/example/test/benchmark targets that already receive liburing usage
     requirements through `conflux_uring`, `conflux_work`, `conflux_file_io`, or
     `conflux_socket_io`.
   - Next concrete slice: start the HTTP handler-model normalization by moving
     blocking user handler execution toward explicit work-pool dispatch instead
     of running arbitrary sync handlers on the ring thread.

2. **Normalize the HTTP handler model.**
   - Target one internal shape: `root::Task<HttpResponse>(HttpRequestView)`.
   - Do not execute arbitrary sync user handlers on the ring thread unless they
     are explicitly marked nonblocking or routed through a work pool.

3. **Use allocation diagnostics before pooling more coroutine frames.**
   - `CONFLUX_WORK_ALLOC_STATS` exists; use it to confirm `root::Task<T>` /
     `ControlBlockModel<T>` pressure before adding the next pool.
   - Avoid jumping straight to P2300.

4. **Close benchmark gaps that unblock decisions.**
   - Add/finish `default_` vs `poll_first` vs adaptive recv-arm benchmarks.
   - Add the end-to-end `SocketTaskRing` vs `FileReader` JSON decode benchmark.
   - Validate send-zc adaptive thresholds before changing defaults.

5. **Finish async cancellation edges.**
   - HTTPS async TLS connect/recv cancellation awareness.
   - Explicit recv cancel for armed server connections.
   - Cancel-by-fd only after per-fd cancel slot tracking is designed.

6. **Split the next HTTP modular target.**
   - `conflux::http_router`, the main router-dependent middleware buckets,
     `conflux::http_compression`, `conflux::http_client`,
     `conflux::http_async_client`, `conflux::http_proxy`, `conflux::http1`,
     `conflux::http2`, `conflux::http3`, `conflux::http_protocol`,
     `conflux::http_server`, `conflux::http_app`, and `conflux::http` now exist.
   - `conflux::http_static`, `conflux::http_static_core`,
     `conflux::http_static_async`, and `conflux::http_realtime` now own static,
     response, SSE, and WebSocket surfaces/implementation helpers. Remaining work:
     collapse router-owned helper leftovers only where doing so removes a real
     dependency edge.

7. **Prototype compile-time JSON only after the return type is documented.**
   - Subset: integers, booleans, null, no-escape strings, nested objects/arrays.
   - No float literals until constexpr number parsing constraints are resolved.

8. **DB follow-ups after the HTTP/runtime contract slices.**
   - COPY API first if benchmark scope is clear.
   - True libpq wire-level pipeline mode remains larger/riskier.

9. **Defer full simdjson-style Stage-0 and P2300 rewrites.**
    - Stage-0 only after business need justifies it; previous whitespace/string
      scan widening failed gates.
    - P2300 is multi-week architecture work and should follow measured
      root-task allocation results.
