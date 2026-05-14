# conflux HTTP Server API Reference

**Module:** `conflux.net.http` (umbrella)  
**Namespace:** `conflux::http`

---

## Quick start

```cpp
import conflux.net.http;

using namespace conflux;
using namespace conflux::http;

auto router = Router{};
router.get("/hello", [](HttpRequestView const&) -> HttpResponse {
    return HttpResponse::text("hello world");
});

ServerConfig cfg{};
cfg.bind = "0.0.0.0:8080";
auto server = HttpServer{cfg, std::move(router)};
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

`HttpRequestView` is the zero-copy view handed to synchronous handlers.
`HttpRequest` is the owned variant used when request data may cross coroutine
suspension or escape the ring-thread handler call.

The `conflux::http` first-contact aliases are:

```cpp
using RequestView  = ::HttpRequestView;
using OwnedRequest = ::HttpRequest;
using Request      = RequestView;  // sync-handler default; borrowed view
using Response     = ::HttpResponse;
```

Use `http::Request` / `HttpRequestView` for short synchronous handlers, and
`http::OwnedRequest` / `HttpRequest` for coroutine handlers or escaped request
data.

```cpp
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string remote_addr;
    bool        is_tls;
    HttpFields  params;
    HttpFields  headers;  // case-insensitive
    HttpFields  query;
    HttpFields  form;
    HttpFields  cookies;
    std::vector<UploadedFile> files;
    std::string body;
};

struct HttpRequestView {
    std::string_view method;
    std::string_view path;
    std::string_view version;
    std::string_view remote_addr;
    bool             is_tls;
    HttpFieldsView   params;
    HttpFieldsView   headers;
    HttpFieldsView   query;
    HttpFieldsView   form;
    HttpFieldsView   cookies;
    std::span<UploadedFile const> files;
    std::string_view body;

    HttpRequest to_owned() const;
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

### Query string and path parameters

Parsed query parameters live in `req.query`. Matched path captures live in
`req.params`.

```cpp
auto name = req.params["name"];
auto page = req.query["page"];
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

    // Context/coroutine routes with access to the ring context.
    template<ContextHandlerFunction F>
    Router& add_context(std::string_view method, std::string_view path, F&& handler);

    // WebSocket upgrade and SSE.
    template<typename F> Router& ws (std::string_view path, F&& handler);
    template<typename F> Router& sse(std::string_view path, F&& handler);

    // Static file serving.
    Router& serve_static(std::string_view url_prefix,
                         std::string root_dir,
                         StaticOptions const& = {});

    // Middleware (applied in registration order, outermost first).
    template<class F> requires ::Middleware<F>
    Router& use(F&&);

    // Route groups.
    template<typename F> Router& group(std::string_view prefix, F&& fn);

    // Error/not-found handlers.
    template<typename F> Router& on_not_found(F&& handler);
    template<typename F> Router& on_error(F&& handler);

    // Route introspection.
    std::vector<RouteInfo> route_infos() const;
};
```

Path patterns support `{param}` segment captures and `*` wildcards. Captures are
accessible through `req.params["param"]`.

The public concepts are intended for user helpers and diagnostics. `HttpRequestView` handlers are sync-only because a view may dangle after coroutine suspension. Async handlers must accept the owning `HttpRequest`. A synchronous handler may also accept `HttpRequest const&`; that deliberately materializes an owned request before the call, so prefer `HttpRequestView const&` unless ownership is needed.

---

## Handlers

Handlers can be synchronous or coroutine-based. View handlers are sync-only.
Coroutine handlers must accept the owning request type.

```cpp
// Sync handler: runs inline on the HTTP ring thread and borrows the request.
router.get("/ping", [](HttpRequestView const& req) -> HttpResponse {
    return HttpResponse::text("pong");
});

// Async handler: the request is owned before the task can suspend.
router.post("/echo", [](HttpRequest const& req) -> root::Task<HttpResponse> {
    co_return HttpResponse::text(req.body);
});

// Explicit worker placement for blocking/heavy work.
auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 2});
router.get("/slow", [pool](HttpRequestView const&) -> HttpResponse {
    return conflux::http::defer(pool, [] {
        return HttpResponse::text("done");
    });
});
```

---

## `HttpResponse`

```cpp
class HttpResponse {
public:
    static HttpResponse text(std::string body);
    static HttpResponse text(std::string body, int status, std::string status_text);
    static HttpResponse html(std::string body);
    static HttpResponse html(std::string body, int status, std::string status_text);
    static HttpResponse json(std::string already_serialized_body);
    static HttpResponse json(std::string already_serialized_body, int status, std::string status_text);
    static HttpResponse redirect(std::string_view location, int status = 302);
    static HttpResponse not_found(std::string_view path = {});
    static HttpResponse bad_request(std::string_view detail = {});
    static HttpResponse unauthorized(std::string_view www_authenticate = {});
    static HttpResponse forbidden(std::string_view detail = {});
    static HttpResponse method_not_allowed(std::initializer_list<std::string_view> allowed = {});
    static HttpResponse unprocessable_entity(std::string_view detail = {});
    static HttpResponse uri_too_long();
    static HttpResponse header_fields_too_large();
    static HttpResponse content_too_large();
    static HttpResponse bad_gateway(std::string_view detail = {});
    static HttpResponse gateway_timeout();
    static HttpResponse no_content();
    static HttpResponse sse(std::shared_ptr<SseChannel>);
    static HttpResponse deferred(std::shared_ptr<DeferredResponse>);
    static HttpResponse internal_error(std::string_view detail = {});
};
```

JSON response bodies are explicit raw strings at this layer. Structured JSON
serialization belongs at the call site or in `conflux.net.http.response_json`
helpers.

---

## SSE (Server-Sent Events)

### SSE route handler

```cpp
// SSE handlers are registered with router.sse(), not router.get().
router.sse("/events", [](HttpRequestView const& req, std::shared_ptr<SseChannel> const& ch) {
    ch->send("data: hello\n\n");
    ch->close();
});
```

### `SseOverflowPolicy`

```cpp
enum class SseOverflowPolicy { DropNewest, DropOldest, Disconnect };
```

### `SseBroadcaster`

```cpp
class SseChannel {
public:
    bool send(std::string frame);
    bool send_view(std::string_view frame);
    bool send_event(std::string_view type, std::string_view data);
    void close();
};

class SseBroadcaster {
public:
    std::shared_ptr<SseChannel> subscribe();
    void broadcast(std::string frame);
};
```

---

## WebSocket

```cpp
router.ws("/ws", [](HttpRequestView const& req, WsConn& ws) {
    while (auto frame = ws.recv()) {
        if (frame->opcode == WsConn::Opcode::Text) {
            ws.send_text(frame->payload);
        }
    }
});
```

`WsConn` provides blocking connection-local operations on the worker that owns the
upgraded connection:

```cpp
std::optional<WsConn::Frame> recv();
bool send_text(std::string_view);
bool send_binary(std::span<std::byte const>);
bool send_ping(std::string_view = {});
void close(uint16_t code = 1000, std::string_view reason = {});
```

WebSocket routing is implemented as a GET route that returns a `WsUpgrade`
response. The router handles the Upgrade/101 handshake transparently.

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
in `conflux.net.http.static_async` / `conflux::http_static_async`. Static
route-registration internals live in `conflux.net.router_static`: the module
exports a concrete handler bundle, while `router_impl` only installs prepared
GET/PUT/DELETE routes. `conflux.net.router` still re-exports the public surface
modules so existing router imports keep working while implementation bulk is
split into smaller package targets.

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

router.serve_static("/assets", "./public", StaticOptions{
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
names advertise calling-thread blocking behavior. See `docs/execution-model.md` for the shared task/executor contract and
`docs/concurrency-naming-model.md` for the code-review naming/placement guide.

CPU pinning: set `ring_core` and `worker_core_base` in `ServerConfig` (see `docs/conflux-http-client-api.md` and `perf_ideas.md`).

---

## JSON route responses

Raw serialized JSON can still use `HttpResponse::json(std::string)`. Structured
values should go through the optional `conflux.net.http.response_json` module so
route code depends on the provider boundary instead of one concrete DOM:

```cpp
import conflux.net.http.response_json;

router.get("/api/count", [](HttpRequestView const &) {
    return conflux::http::json::response_or_internal_error(static_cast<i64>(42));
});

router.post("/api/items", [](HttpRequestView const &) {
    auto resp = conflux::http::json::try_response(
        ItemCreated{.id = 7},
        {.status = kHttpCreated, .status_text = "Created"});
    return resp ? std::move(*resp) : HttpResponse::internal_error();
});
```

`try_response` returns `std::expected<HttpResponse, json::boundary::Error>` and
preserves provider-neutral serialization failures. `response_or_internal_error`
is the route ergonomic helper for handlers that must return `HttpResponse`; it
returns a fixed JSON 500 body when serialization fails.

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
router.on_not_found([](HttpRequestView const& req) -> HttpResponse {
    return HttpResponse::json(R"({"error":"not found"})", 404, "Not Found");
});

router.on_error([](HttpRequestView const& req, std::exception const& ex) -> HttpResponse {
    // log ex, return 500
    return HttpResponse::internal_error(ex.what());
});
```
