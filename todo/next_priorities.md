# Next Implementation Priorities

This list is intentionally ordered so future patch work can pick the first open
item instead of re-deciding from scratch. It is based on the current `todo/`,
`proposals/`, and top-level design documents in this source snapshot.

## Selection rules

1. Prefer contract-breaking modularity and API-boundary fixes before deeper perf
   work; this is still pre-v1, so API breakage is acceptable when it improves
   ergonomics or performance.
2. Prefer small target-split and dependency-boundary patches over large rewrites.
3. Only implement benchmark-gated performance changes when the benchmark harness
   or measurement target already exists.
4. Defer speculative architecture rewrites until allocation/benchmark data proves
   the current path is the bottleneck.

## Ordered list

1. **Finish modular CMake target slices.**
   - Current status: `core`, `json`, `file_io_sync`, `file_map`, `file_io`,
     `socket_io`, `utils`, `net_config`, `net_cancel`, `net_tls`, `http_core`,
     `http_json`, `template`, and `template_watch` have started splitting out.
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
   - Next concrete slice: split `conflux::http_compression` and HTTP client/proxy
     support modules, or go directly to `conflux::http_server` if the remaining
     server implementation dependencies are cleaner after compression/client split.

2. **Remove remaining stale dependency edges from proposal reviews.**
   - Re-check `conflux_socket_io -> file_io`, `net.client -> file_io`, and
     `HttpRequest::Builder::body_json(NodeRef)` before each modular patch; these
     have been moving and proposals can be stale.
   - Keep `file_io_sync` and `file_map` usable without `liburing`.

3. **Normalize the HTTP handler model.**
   - Target one internal shape: `root::Task<HttpResponse>(HttpRequestView)`.
   - Do not execute arbitrary sync user handlers on the ring thread unless they
     are explicitly marked nonblocking or routed through a work pool.

4. **Use allocation diagnostics before pooling more coroutine frames.**
   - `CONFLUX_WORK_ALLOC_STATS` exists; use it to confirm `root::Task<T>` /
     `ControlBlockModel<T>` pressure before adding the next pool.
   - Avoid jumping straight to P2300.

5. **Close benchmark gaps that unblock decisions.**
   - Add/finish `default_` vs `poll_first` vs adaptive recv-arm benchmarks.
   - Add the end-to-end `SocketTaskRing` vs `FileReader` JSON decode benchmark.
   - Validate send-zc adaptive thresholds before changing defaults.

6. **Finish async cancellation edges.**
   - HTTPS async TLS connect/recv cancellation awareness.
   - Explicit recv cancel for armed server connections.
   - Cancel-by-fd only after per-fd cancel slot tracking is designed.

7. **Split the next HTTP modular target.**
   - `conflux::http_router` and the main router-dependent middleware buckets now
     exist.
   - Next try `conflux::http_compression` plus HTTP client/proxy support modules,
     then split `conflux::http_server` once the remaining implementation imports
     are mostly target-backed.
   - Prefer static-core/static-async only if a small no-liburing metadata/mmap slice
     is clearly separable from streamed file serving.

8. **Prototype compile-time JSON only after the return type is documented.**
   - Subset: integers, booleans, null, no-escape strings, nested objects/arrays.
   - No float literals until constexpr number parsing constraints are resolved.

9. **DB follow-ups after the HTTP/runtime contract slices.**
   - COPY API first if benchmark scope is clear.
   - True libpq wire-level pipeline mode remains larger/riskier.

10. **Defer full simdjson-style Stage-0 and P2300 rewrites.**
    - Stage-0 only after business need justifies it; previous whitespace/string
      scan widening failed gates.
    - P2300 is multi-week architecture work and should follow measured
      root-task allocation results.
