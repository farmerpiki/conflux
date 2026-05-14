# conflux HTTP Server API Reference

**Module:** `conflux.net.http` (umbrella)  
**Namespace:** `conflux::http`

---

## Quick start

```cpp
import conflux.net.http;

auto router = conflux::http::Router{};
router.get("/hello", [](HttpRequestView req) -> HttpResponse {
    return HttpResponse::ok().body("hello world").build();
});

conflux::http::ServerConfig cfg{};
cfg.bind = "0.0.0.0:8080";
auto server = conflux::http::HttpServer{cfg, std::move(router)};
server.run();
```

---

## Server Metrics

`HttpServer::metrics()` returns a passive snapshot of counters accumulated by all
rings. It is intended to be called after `run()` returns; it is not synchronized
against active ring threads.

```cpp
struct SendZcMetrics {
    u64 attempts;
    u64 bytes_requested;
    u64 bytes_sent;
    u64 notifications;
    u64 copied_notifications;
    u64 sends_without_notification;
    u64 errors_enomem;
    u64 errors_other;
    u64 fallback_regular_send;
    u64 adaptive_disable_count;
};

struct HttpServerMetrics {
    u64 sq_dropped;
    u64 cq_overflow;
    u64 accepted_direct_failures;
    u64 zc_notifications_pending;
    u64 recv_bundle_cqes;
    u64 recv_bundle_slices;
    u64 recv_bundle_bytes;
    SendZcMetrics send_zc;
};

HttpServerMetrics HttpServer::metrics() const noexcept;
```

The snapshot covers io_uring pressure (`sq_dropped`, `cq_overflow`), direct-accept
fallbacks, pending SEND_ZC notifications, recv-bundle effectiveness, and SEND_ZC
usage/copy/error/adaptive-disable counters.

---

## Hardened request limits

`Config` defaults are bounded for web-facing use: 1 MiB request bodies, 30 s
request timeout, 10 s TLS/plain sniff timeout, 8 KiB request lines, 8 KiB
single header lines, 100 headers, 64 KiB aggregate header blocks, and 100000
chunked body chunks. The parser enforces request-line and header-line/count
limits incrementally, before a client has to send the final `\r\n\r\n` header
terminator.

INI configuration exposes these knobs in `[server]`:

```ini
max_body_size = 1048576
request_timeout_ms = 30000
tls_sniff_timeout_ms = 10000
max_request_line_size = 8192
max_header_line_size = 8192
max_headers = 100
max_header_block_size = 65536
max_chunks = 100000
```

HTTP/3 has a separate `[http3].max_body_size` knob; it defaults to the same
1 MiB cap as HTTP/1 request bodies.

---

## `HttpRequest` / `HttpRequestView`

`HttpRequestView` is the zero-copy view handed to handlers. `HttpRequest` is the owned variant (used when the handler must outlive the request coroutine).

```cpp
struct HttpRequestView {
    std::string_view       method;
    std::string_view       path;
    std::string_view       raw_query;
    HttpFields             headers;     // case-insensitive
    std::string_view       body;

    HttpFields             form;        // parsed from application/x-www-form-urlencoded body
    HttpFields             cookies;     // parsed from Cookie header
    std::vector<UploadedFile> files;    // parsed from multipart/form-data body
};
```

### `UploadedFile`

```cpp
struct UploadedFile {
    std::string_view name;          // form field name
    std::string_view filename;      // original filename from Content-Disposition
    std::string_view content_type;
    std::string_view data;          // file bytes (borrowed from request body)

    UploadedFile to_owned() const;  // copies all views into owned strings
};
```

`data` and `filename` borrow from the request body. They are valid only for the handler's lifetime. Call `to_owned()` if you need them beyond the handler return.

### Query string

```cpp
// Parse query params on demand:
auto params = req.parse_query();   // returns HttpFields
auto val    = params["name"];
```

---

## Router

```cpp
using NextHandler = CloneableFunction<HttpResponse(HttpRequestView const&)>;
using MiddlewareFunction = CloneableFunction<HttpResponse(HttpRequestView const&, NextHandler const&)>;

template<class R>
concept HandlerResult = std::same_as<R, HttpResponse>
                     || std::same_as<R, root::Task<HttpResponse>>;

template<class F>
concept ViewHandler = requires(std::decay_t<F>& fn, HttpRequestView const& req) {
    { std::invoke(fn, req) } -> std::same_as<HttpResponse>;
};

template<class F>
concept RequestHandler = requires(std::decay_t<F>& fn, HttpRequest const& req) {
    { std::invoke(fn, req) } -> HandlerResult;
};

template<class F> concept RouteHandler = ViewHandler<F> || RequestHandler<F>;

template<class F>
concept ContextHandlerFunction = requires(std::decay_t<F>& fn,
                                          HttpRequest const& req,
                                          RequestContext const& ctx) {
    { std::invoke(fn, req, ctx) } -> std::same_as<root::Task<HttpResponse>>;
};

template<class F>
concept ViewMiddleware = requires(std::decay_t<F>& fn,
                                  HttpRequestView const& req,
                                  NextHandler const& next) {
    { std::invoke(fn, req, next) } -> std::same_as<HttpResponse>;
};

template<class F>
concept RequestMiddleware = requires(std::decay_t<F>& fn,
                                     HttpRequest const& req,
                                     NextHandler const& next) {
    { std::invoke(fn, req, next) } -> std::same_as<HttpResponse>;
};

template<class F> concept Middleware = ViewMiddleware<F> || RequestMiddleware<F>;

class Router {
public:
    using Handler = NextHandler;
    using Middleware = MiddlewareFunction;

    // Route registration
    template<RouteHandler F> Router& get    (std::string_view path, F&&);
    template<RouteHandler F> Router& post   (std::string_view path, F&&);
    template<RouteHandler F> Router& put    (std::string_view path, F&&);
    template<RouteHandler F> Router& patch  (std::string_view path, F&&);
    template<RouteHandler F> Router& del    (std::string_view path, F&&);
    template<RouteHandler F> Router& options(std::string_view path, F&&);

    // WebSocket upgrade
    template<typename F>
    Router& ws(std::string_view path, F&& handler);

    // Static file serving
    Router& static_files(std::string_view path_prefix,
                         std::filesystem::path root,
                         StaticOptions const& = {});

    // Middleware (applied in registration order, outermost first)
    template<class F> requires ::Middleware<F>
    Router& use(F&&);

    // Sub-routers
    Router& mount(std::string_view prefix, Router sub);

    // Error/not-found handlers
    Router& not_found(Handler);
    Router& error_handler(std::function<HttpResponse(HttpRequestView const&, std::exception_ptr)>);

    // Route introspection
    std::vector<RouteInfo> route_infos() const;
};
```

Path patterns support `:param` (single segment) and `*` (wildcard). Path parameters are accessible via `req.path_params["param"]`.

The public concepts are intended for user helpers and diagnostics. `HttpRequestView` handlers are sync-only because a view may dangle after coroutine suspension. Async handlers must accept the owning `HttpRequest`.

---

## Handlers

Handlers can be synchronous or coroutine-based:

```cpp
// Sync handler
router.get("/ping", [](HttpRequestView req) -> HttpResponse {
    return HttpResponse::ok().body("pong").build();
});

// Async handler (coroutine)
router.post("/echo", [](HttpRequestView req) -> root::Task<HttpResponse> {
    co_return HttpResponse::ok().body(req.body).build();
});

// Deferred / streaming response
router.get("/slow", [](HttpRequestView req) -> DeferredResponse {
    return [](ResponseWriter& w) -> root::Task<void> {
        co_await w.send_headers(200, {});
        co_await w.send_chunk("chunk 1");
        co_await w.send_chunk("chunk 2");
        co_await w.finish();
    };
});
```

---

## `HttpResponse`

```cpp
class HttpResponse {
public:
    static Builder ok();           // 200
    static Builder created();      // 201
    static Builder no_content();   // 204
    static Builder bad_request();  // 400
    static Builder not_found();    // 404
    static Builder internal_error(); // 500
    static Builder status(int code);

    // Shorthand helpers
    static HttpResponse bad_gateway();
    static HttpResponse service_unavailable();
};

// Current response helpers are value factories/mutators rather than a nested
// builder class. JSON response bodies are explicit raw strings; structured JSON
// serialization belongs at the call site or in conflux.net.http.json helpers.
HttpResponse::html(std::string body);
HttpResponse::json(std::string already_serialized_body);
HttpResponse::text(std::string body);
HttpResponse::not_found(std::string_view path = {});
```

---

## SSE (Server-Sent Events)

### SSE route handler

```cpp
// SSE handler receives an SseWriter
router.get("/events", [&broadcaster](HttpRequestView req, SseWriter sse) -> root::Task<void> {
    auto sub = broadcaster.subscribe();
    while (true) {
        auto event = co_await sub.next();
        if (!event) break;
        co_await sse.send(*event);
    }
});
```

### `SseOverflowPolicy`

```cpp
enum class SseOverflowPolicy { DropNewest, DropOldest, Disconnect };
```

### `SseBroadcaster`

```cpp
class SseBroadcaster {
public:
    explicit SseBroadcaster(SseBroadcasterOptions const&);

    void broadcast(SseEvent const&);   // non-blocking; applies overflow policy
    Subscription subscribe();
};

struct SseBroadcasterOptions {
    size_t          queue_depth{64};
    SseOverflowPolicy overflow{SseOverflowPolicy::DropOldest};
};

struct SseEvent {
    std::optional<std::string> id;
    std::optional<std::string> event;
    std::string                data;
    std::optional<std::chrono::milliseconds> retry;
};
```

---

## WebSocket

```cpp
router.ws("/ws", [](WsStream ws) -> root::Task<void> {
    while (true) {
        auto frame = co_await ws.recv();
        if (!frame || frame->is_close()) break;
        co_await ws.send(WsFrame::text(frame->payload()));
    }
});
```

`WsStream` provides:
```cpp
root::Task<std::optional<WsFrame>> recv();
root::Task<void>                   send(WsFrame);
root::Task<void>                   close(uint16_t code = 1000, std::string_view reason = {});
```

WebSocket routing is implemented as a GET route that returns a `WsUpgrade` response. The router handles the Upgrade/101 handshake transparently.

---

## Static/realtime component modules

`StaticOptions` is exported by `conflux.net.http.static_files` / `conflux::http_static`.
Server request vocabulary (`UploadedFile`, `HttpRequest`, `HttpRequestView`,
`CloneableFunction`) is exported by `conflux.net.http.server_types`, which is
part of `conflux::http_core`. SSE and WebSocket types/helpers (`SseOverflowPolicy`,
`SseChannel`, `SseBroadcaster`, `WsConn`, `WsUpgrade`) are exported by
`conflux.net.http.realtime` / `conflux::http_realtime`. HTTP response
vocabulary (`HttpResponse`, `DeferredResponse`, mapped/streamed body carriers)
is exported by `conflux.net.http.response` / `conflux::http_response`. Static
path/cache helpers live in `conflux.net.http.static_core` /
`conflux::http_static_core`. Static root-dir ownership, contained `openat2`
probing, GET/PUT/DELETE execution paths, and async file helper coroutines live
in `conflux.net.http.static_async` / `conflux::http_static_async`.
`conflux.net.router` still re-exports the public surface modules so existing
router imports keep working while implementation bulk is split into smaller
package targets.

## Static file serving

```cpp
struct StaticOptions {
    std::string              cache_control{"max-age=3600, public"};
    bool                     precompressed{true};      // serve .gz/.br sidecar if present
    bool                     directory_listing{false};
    std::shared_ptr<WorkPool> offload_pool{};          // explicit worker placement for blocking file reads
    StaticFileCacheConfig    file_cache{};             // in-memory cache; disabled by default
    bool                     allow_put{false};
    bool                     allow_delete{false};
};

router.static_files("/assets", "./public", StaticOptions{
    .precompressed = true,
    .cache_control = "max-age=86400, public",
});
```

Precompressed serving: if `Accept-Encoding: gzip` is present and `file.gz` exists next to `file`, the `.gz` sidecar is served directly with `Content-Encoding: gzip`. Same for `.br`. No runtime compression.

`allow_put` / `allow_delete` enable write operations on the served directory. Off by default.

---

## Middleware

Password storage uses the dedicated `conflux.net.password_hash` boundary; see `docs/auth-password-hashing.md` for Argon2id/PBKDF2 formats and login-time rehash migration.

Middleware wraps every matched route. Applied outermost-first in registration order.

```cpp
router.use([](HttpRequestView req, NextHandler next) -> HttpResponse {
    // pre-processing
    auto resp = next(req);
    // post-processing
    return resp;
});
```

### Built-in: access log

```cpp
router.use(make_access_log_middleware([](std::string const& line) {
    std::println("{}", line);
}));
// Logs: [ISO8601] METHOD path status bytes elapsed_ms
```

---

## OpenAPI / Route metadata

```cpp
struct RouteInfo {
    std::string              method;
    std::string              path_pattern;  // OpenAPI style: /users/{id}
    std::vector<std::string> path_params;   // captured parameter names
};

auto infos = router.route_infos();
// Use with conflux.net.openapi to generate an OpenAPI spec document
```

`conflux.net.openapi` module consumes `route_infos()` and produces an OpenAPI 3.x JSON document. Import separately — not included in the `conflux.net.http` umbrella by default.

---

## Concurrency model

Each `SocketTaskRing` ring thread runs an independent io_uring loop. HTTP
handlers execute on the ring thread. This is the server execution contract, not a
compatibility detail to hide with automatic offload.

Synchronous handlers must be short, bounded, and non-blocking. Blocking disk I/O,
DNS, blocking client calls, database calls, sleeps, and heavy CPU work stall the
ring and must be made explicit through coroutine suspension, caller-visible
executor/work-pool placement, or raw syscall-style helpers whose `blocking_*`
names advertise calling-thread blocking behavior. See `docs/execution-model.md`
for the shared task/executor and naming contract.

CPU pinning: set `ring_core` and `worker_core_base` in `ServerConfig` (see `docs/conflux-http-client-api.md` and `perf_ideas.md`).

---

## Graceful shutdown

```cpp
server.request_shutdown(); // async-signal-safe: wake rings via eventfd only
server.shutdown();         // normal thread-side graceful shutdown
server.join();     // waits for all ring threads to exit
```

In-flight requests are allowed to complete. New accepts stop immediately. A configurable drain timeout closes connections that exceed it.

---

## Error handlers

```cpp
router.not_found([](HttpRequestView req) -> HttpResponse {
    return HttpResponse::json(R"({"error":"not found"})", 404, "Not Found");
});

router.error_handler([](HttpRequestView req, std::exception_ptr ep) -> HttpResponse {
    // log ep, return 500
    return HttpResponse::internal_error();
});
```
