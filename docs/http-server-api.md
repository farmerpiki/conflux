# conflux HTTP Server API Reference

**Module:** `conflux.http` (first-contact facade; `conflux.net.http.server` remains the lower-level server surface)
**Namespace:** `conflux::http`

---

## Quick start

```cpp
import std;
import conflux.http;

namespace http = conflux::http;

int main() {
    auto app = http::app();
    app.get("/hello", [](http::RequestView const&) {
        return http::text("hello world");
    });

    return http::exit_code(http::run(std::move(app), {.port = 8080}));
}
```

Use `Router` + `HttpServer` directly when you need lower-level server ownership;
use `http::App` for first-contact routes, middleware, WebSocket/SSE, and static
file registration.

For request/response copy costs, borrow lifetimes, coroutine ownership rules,
and zero-copy caveats, see [`cost-lifetime-model.md`](cost-lifetime-model.md).

---

## Fallible setup factories

`HttpServer` construction can allocate eventfds and initialize TLS contexts before `run()`. Use `try_create` when setup errors should be reported as values rather than exceptions.

```cpp
auto server = HttpServer::try_create(cfg, std::move(router));
if (!server) {
    std::println(std::cerr, "server setup failed: {}", server.error());
    return 1;
}
return static_cast<int>((*server)->run());
```

The `App` facade mirrors this with `try_server()` and `try_run()`.

```cpp
auto status = std::move(app).try_run({.port = 8080});
if (!status) {
    std::println(std::cerr, "server setup failed: {}", status.error());
    return 1;
}
return static_cast<int>(*status);
```

`try_config_from_ini(path)` provides the same value-returning style for config load/parse errors. `config_from_ini_checked(path)` remains as the older expected-returning spelling, and `config_from_ini(path)` throws `std::runtime_error` on failure.

### App facade passthroughs

`http::App` keeps the route-registration APIs commonly needed before handing ownership to `try_server()` or `run()`. Use `app.add(method, path, handler)` for custom HTTP methods, ordinary verbs for handlers that need `RequestContext`, `app.use(...)` for both sync and owned async middleware, and `app.routes()` / `app.openapi_spec()` for app-level metadata. The raw router is an extended escape hatch available as `http::router(app)` after `import conflux.http.extended;`.

```cpp
app.add("REPORT", "/reports/{id}", [](http::RequestView const& req) {
    return http::Response::text(std::string{req.params["id"]});
});

auto routes = app.routes();
```

---

## Server Metrics

`HttpServer::metrics()` returns a passive snapshot of counters accumulated by all
rings. It is intended to be called after `run()` returns; it is not synchronized
against active ring threads.

```cpp
struct SendZcMetrics {
    u64 attempts;
    u64 plain_attempts;
    u64 mapped_attempts;
    u64 bytes_requested;
    u64 bytes_sent;
    u64 notifications;
    u64 copied_notifications;
    u64 sends_without_notification;
    u64 errors_enomem;
    u64 errors_other;
    u64 fallback_regular_send;
    u64 tls_bypass;
    u64 tls_bypass_bytes;
    u64 adaptive_disable_count;
};

enum class HttpRejectReason : u8 {
    none,
    malformed_request,
    request_line_too_large,
    header_line_too_large,
    header_block_too_large,
    too_many_headers,
    missing_host,
    duplicate_host,
    malformed_content_length,
    duplicate_content_length,
    content_length_with_transfer_encoding,
    unsupported_transfer_encoding,
    invalid_transfer_encoding,
    invalid_chunk,
    body_too_large,
    expectation_failed,
    header_timeout,
    body_timeout,
};

string_view reject_reason_code(HttpRejectReason) noexcept;
int reject_reason_status(HttpRejectReason) noexcept;
string_view reject_reason_detail(HttpRejectReason) noexcept;

struct HttpRejectionMetrics {
    u64 malformed_request;
    u64 request_line_too_large;
    u64 header_line_too_large;
    u64 header_block_too_large;
    u64 too_many_headers;
    u64 missing_host;
    u64 duplicate_host;
    u64 malformed_content_length;
    u64 duplicate_content_length;
    u64 content_length_with_transfer_encoding;
    u64 unsupported_transfer_encoding;
    u64 invalid_transfer_encoding;
    u64 invalid_chunk;
    u64 body_too_large;
    u64 expectation_failed;
    u64 header_timeout;
    u64 body_timeout;
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
    HttpRejectionMetrics rejections;
    HttpPressureMetrics pressure;
};

HttpServerMetrics HttpServer::metrics() const noexcept;
```

The snapshot covers io_uring pressure (`sq_dropped`, `cq_overflow`), direct-accept
fallbacks, pending SEND_ZC notifications, recv-bundle effectiveness, and SEND_ZC
usage/copy/error/adaptive-disable counters. `plain_attempts` and
`mapped_attempts` split successful SEND_ZC submission attempts by response source.
`fallback_regular_send` counts failed SEND_ZC submissions that fell back to the
regular send path. `tls_bypass` / `tls_bypass_bytes` count large TLS responses
that crossed the SEND_ZC threshold but intentionally used the TLS send path
instead of SEND_ZC, so benchmark runs can separate copy-notification behavior
from TLS-incompatible fallback policy.

Pressure counters live under `HttpServerMetrics::pressure`:

```cpp
struct HttpPressureMetrics {
    u64 accept_rejected;
    u64 connections_closed_for_pressure;
    u64 response_backpressure_events;
    u64 sse_dropped_newest;
    u64 sse_dropped_oldest;
    u64 sse_disconnected_for_pressure;
    u64 websocket_closed_for_pressure;
    u64 drain_started;
    u64 drain_deadline_hit;
    u64 drain_forced_close;
};
```

The snapshot currently covers server-owned drain/accept/WebSocket pressure
directly. Application-owned SSE channels expose per-channel pressure counters
through `SseChannel::pressure_metrics()`; aggregate those in application metrics
when channels are created outside the server.

`conflux.net.metrics` also exposes
`format_pressure_metrics_prometheus(HttpPressureMetrics const&)` for apps that
want to append server pressure counters to an existing Prometheus response.

HTTP/1 parser and dispatch rejections increment passive server counters under
`HttpServerMetrics::rejections`; they are server metrics, not middleware
metrics, and do not require installing application middleware. Classified
HTTP/1 rejection responses use `application/problem+json` with stable
snake-case `code` strings and non-sensitive `detail` text.

| Reason | Status | Counter |
|---|---:|---|
| `malformed_request` | 400 | `rejections.malformed_request` |
| `request_line_too_large` | 414 | `rejections.request_line_too_large` |
| `header_line_too_large` | 431 | `rejections.header_line_too_large` |
| `header_block_too_large` | 431 | `rejections.header_block_too_large` |
| `too_many_headers` | 431 | `rejections.too_many_headers` |
| `missing_host` | 400 | `rejections.missing_host` |
| `duplicate_host` | 400 | `rejections.duplicate_host` |
| `malformed_content_length` | 400 | `rejections.malformed_content_length` |
| `duplicate_content_length` | 400 | `rejections.duplicate_content_length` |
| `content_length_with_transfer_encoding` | 400 | `rejections.content_length_with_transfer_encoding` |
| `unsupported_transfer_encoding` | 400 | `rejections.unsupported_transfer_encoding` |
| `invalid_transfer_encoding` | 400 | `rejections.invalid_transfer_encoding` |
| `invalid_chunk` | 400 | `rejections.invalid_chunk` |
| `body_too_large` | 413 | `rejections.body_too_large` |
| `expectation_failed` | 417 | `rejections.expectation_failed` |
| `header_timeout` | 408 | `rejections.header_timeout` |
| `body_timeout` | 408 | `rejections.body_timeout` |

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

`Config::feature_fallback` controls how requested optional runtime features are
handled when host capabilities do not match the config:

- `fail_fast`: startup validation rejects explicit capability misses.
- `warn_and_fallback`: validation reports issues and the server uses supported
  paths.
- `silent_fallback`: supported paths are used without public warning.

Use `Config::summary_redacted()` or `Config::to_json_redacted()` for effective
configuration diagnostics. Secret-like fields are redacted. `HttpServer` also
exposes `startup_report()` for pull-based startup diagnostics; the library does
not print this report by default.

---

## Server request and response types

The first-contact server namespace exposes the canonical request and response vocabulary:

- `http::Request` / `http::RequestView`: zero-copy request view for handlers and middleware;
- `http::OwnedRequest`: explicit owned copy for escaped request data;
- `http::Response`: server response builder/factory type;
- `http::RunStatus` / `http::ServerMetrics`: server run result and metric snapshot types.

```cpp
struct Request {
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

struct RequestView {
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

    Request to_owned() const;
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

For parsed scalar access, use the typed helpers on `http::RequestView` or
`http::OwnedRequest`. They return `std::expected<T, HttpFieldError>` and allocate error
strings only on failure; successful numeric/bool parsing stays borrowed and
`std::from_chars`-based.

```cpp
router.get("/items/{id}", [](http::RequestView const& req) -> http::Response {
    auto id = req.param_as<std::uint64_t>("id");
    if (!id) return http::Response::text("bad id", 400);

    auto page = req.optional_query_as<std::uint32_t>("page");
    if (!page) return http::Response::text(page.error().message, 400);

    auto debug = req.optional_header_as<bool>("x-debug");
    if (!debug) return http::Response::text(debug.error().message, 400);

    return http::Response::text(std::format("id={} page={} debug={}",
        *id,
        page->value_or(1),
        debug->value_or(false)));
});
```

Available request helpers:

```cpp
req.param_as<T>(name);             req.optional_param_as<T>(name);
req.header_as<T>(name);            req.optional_header_as<T>(name);
req.query_as<T>(name);             req.optional_query_as<T>(name);
req.form_as<T>(name);              req.optional_form_as<T>(name);
req.cookie_as<T>(name);            req.optional_cookie_as<T>(name);
```

Supported targets are `std::string_view`, `std::string`, `bool`, integral types,
and floating-point types. Missing required fields produce
`HttpFieldErrorKind::missing`; malformed values produce `invalid`, `empty`, or
`out_of_range` with `source`, `name`, `value`, and `message` populated.

---

## Router

```cpp
using NextHandler = CloneableFunction<Response(RequestView const&)>;
using MiddlewareFunction = CloneableFunction<Response(RequestView const&, NextHandler const&)>;

template<class R>
concept HandlerResult = std::same_as<R, Response>
                     || std::same_as<R, root::Task<Response>>;

template<class F>
concept ViewHandler = requires(std::decay_t<F>& fn, RequestView const& req) {
    { std::invoke(fn, req) } -> HandlerResult;
};

template<class F>
concept RequestHandler = requires(std::decay_t<F>& fn, OwnedRequest const& req) {
    { std::invoke(fn, req) } -> HandlerResult;
};

template<class F> concept RouteHandler = ViewHandler<F> || RequestHandler<F>;

template<class F>
concept ContextHandlerFunction = requires(std::decay_t<F>& fn,
                                          RequestView const& req,
                                          RequestContext const& ctx) {
    { std::invoke(fn, req, ctx) } -> std::same_as<root::Task<Response>>;
};

template<class F>
concept ContextMiddlewareFunction = requires(std::decay_t<F>& fn,
                                             RequestView const& req,
                                             RequestContext const& ctx,
                                             ContextNextHandler const& next) {
    { std::invoke(fn, req, ctx, next) } -> std::same_as<root::Task<Response>>;
};

template<class F> concept AsyncMiddleware = ContextMiddlewareFunction<F>;

template<class F>
concept ViewMiddleware = requires(std::decay_t<F>& fn,
                                  RequestView const& req,
                                  NextHandler const& next) {
    { std::invoke(fn, req, next) } -> std::same_as<Response>;
};

template<class F>
concept RequestMiddleware = requires(std::decay_t<F>& fn,
                                     OwnedRequest const& req,
                                     NextHandler const& next) {
    { std::invoke(fn, req, next) } -> std::same_as<Response>;
};

template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F> || AsyncMiddleware<F>;

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
    template<ContextHandlerFunction F> Router& get_context    (std::string_view path, F&&);
    template<ContextHandlerFunction F> Router& post_context   (std::string_view path, F&&);
    template<ContextHandlerFunction F> Router& put_context    (std::string_view path, F&&);
    template<ContextHandlerFunction F> Router& patch_context  (std::string_view path, F&&);
    template<ContextHandlerFunction F> Router& del_context    (std::string_view path, F&&);
    template<ContextHandlerFunction F> Router& options_context(std::string_view path, F&&);

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
    template<AsyncMiddleware F>
    Router& use_async(F&&);

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

Fixed-string app routes can tag path parameter types and pass them directly as
plain handler arguments by capture order:

```cpp
app.get<"/todos/{id:i64}">([](std::int64_t id) {
    return http::text(std::format("todo={}", id));
});
```

The public concepts are intended for user helpers and diagnostics. The HTTP
server owns the request storage for the dispatch lifetime; handlers,
middleware, async context routes, and extractors receive `http::Request` /
`http::RequestView` unless they explicitly ask for `http::OwnedRequest`.

Typed app handlers can receive PATCH JSON bodies directly. `http::JsonPatch`
requires `Content-Type: application/json-patch+json`, parses the body, and
validates RFC 6902 operation shape before invoking the handler. `http::MergePatch`
requires `application/merge-patch+json`. Both use the route/app JSON body limit,
return `application/problem+json` on extractor failure, and record the matching
PATCH content type in OpenAPI metadata.

---

## Handlers

Handlers can be synchronous or coroutine-based. Prefer `Request` /
`RequestView`; accepting `OwnedRequest` explicitly materializes an owned copy.
Both sync and async handler bodies start on the HTTP ring thread. Conflux does
not silently offload blocking work: disk I/O, DNS, DB calls, client HTTP,
sleeps, and heavy CPU work must move through an explicit async API or an
explicit worker/offload path.

Request bodies are bounded and buffered in memory today. Raise `max_body_size`
deliberately for known workloads; arbitrary large uploads need the deferred
streaming upload API rather than hidden spill-to-disk behavior.

```cpp
// Sync handler: runs inline on the HTTP ring thread and borrows the request.
router.get("/ping", [](http::RequestView const& req) -> http::Response {
    return http::Response::text("pong");
});

// Async handler: also borrows the server-owned request storage.
router.post("/echo", [](http::RequestView const& req) -> root::Task<http::Response> {
    co_return http::Response::text(req.body);
});

// Explicit worker placement for blocking/heavy work.
auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 2});
router.get("/slow", [pool](http::RequestView const&) -> http::Response {
    return http::offload(pool, [] {
        return http::Response::text("done");
    });
});
```

---

## Lifecycle and pressure

`HttpServer::drain(options)` is the explicit graceful-shutdown entry point. It
uses the same ring wakeup path as `shutdown()`, but records drain intent and
pressure counters so shutdown behavior is visible in metrics.

```cpp
http::DrainOptions opts{
    .deadline = 30s,
    .stop_accepting = true,
    .close_idle = true,
    .finish_requests = true,
    .finish_streams = false,
    .websocket_policy = http::DrainStreamPolicy::close_with_reason,
    .sse_policy = http::DrainStreamPolicy::close_with_retry,
};
auto report = server.drain(opts);
```

`shutdown()` remains the convenience wrapper for ordinary stop requests.
`app.run(...)`, `app.try_run(...)`, and `http::run(...)` keep their existing
behavior. `app.listen(...)` still returns a constructed `HttpServer` through the
fallible setup path; call `run()` on another thread when a controller thread
needs to call `port()`, `metrics()`, `drain()`, or `shutdown()`.

| Situation | Default behavior | Config knob | Metric |
|---|---|---|---|
| New connection while draining | Stop accepting; close any accepted late socket | `DrainOptions::stop_accepting` | `pressure.accept_rejected` |
| Idle keep-alive during drain | Close | `DrainOptions::close_idle` | `DrainReport::idle_closed` |
| Active normal request during drain | Let current send finish until deadline | `DrainOptions::finish_requests`, `deadline` | `DrainReport::requests_finished`, `pressure.drain_deadline_hit` |
| SSE channel full | Drop newest by default | `SseOverflowPolicy` / `OverflowPolicy` vocabulary | `SseChannel::pressure_metrics()` |
| SSE stream during drain | Close unless stream finishing is requested | `DrainOptions::finish_streams`, `sse_policy` | `DrainReport::streams_closed` |
| WebSocket during drain | Close/handoff path is pressure-accounted | `DrainOptions::websocket_policy` | `pressure.websocket_closed_for_pressure` |
| Slow streaming client | Close at drain deadline | `DrainOptions::deadline` | `pressure.drain_forced_close` |
| Ring CQ pressure | Report kernel CQ overflow and fatal overflow status | Ring sizing / runtime config | `cq_overflow`, fatal `RunStatus` |

The shared overflow vocabulary is:

```cpp
enum class OverflowPolicy : std::uint8_t {
    reject,
    drop_oldest,
    drop_newest,
    close_connection,
    backpressure,
};
```

SSE keeps the narrower `SseOverflowPolicy` spelling because it has an existing
channel API. `DropNewest`, `DropOldest`, and `Disconnect` map to
`drop_newest`, `drop_oldest`, and `close_connection`.

HTTP/1.1 drain behavior is implemented by the server ring shutdown path. HTTP/2
and HTTP/3 keep their existing experimental behavior in this branch; full GOAWAY
or QUIC drain correctness should be treated as separate protocol work.

## `http::Response`

```cpp
class Response {
public:
    static std::string_view status_text_for(int status) noexcept;
    static Response with_body(std::string body, std::string content_type);
    static Response with_body(std::string body, std::string content_type, int status);
    static Response with_body(std::string body, std::string content_type, int status, std::string status_text);
    static Response text(std::string body);
    static Response text(std::string body, int status);
    static Response text(std::string body, int status, std::string status_text);
    static Response html(std::string body);
    static Response html(std::string body, int status);
    static Response html(std::string body, int status, std::string status_text);
    static Response json(std::string already_serialized_body);
    static Response json(std::string already_serialized_body, int status);
    static Response json(std::string already_serialized_body, int status, std::string status_text);
    static Response redirect(std::string_view location, int status = 302);
    static Response not_found(std::string_view path = {});
    static Response bad_request(std::string_view detail = {});
    static Response unauthorized(std::string_view www_authenticate = {});
    static Response forbidden(std::string_view detail = {});
    static Response method_not_allowed(std::initializer_list<std::string_view> allowed = {});
    static Response unprocessable_entity(std::string_view detail = {});
    static Response uri_too_long();
    static Response header_fields_too_large();
    static Response content_too_large();
    static Response bad_gateway(std::string_view detail = {});
    static Response gateway_timeout();
    static Response no_content();
    static Response sse(std::shared_ptr<SseChannel>);
    static Response deferred(std::shared_ptr<DeferredResponse>);
    static Response internal_error(std::string_view detail = {});
};
```

`http::Response::text/html/json(body, status)` fills the standard reason phrase
for known status codes, so `text("bad", 400)` is enough for ordinary errors.
Use the three-argument overload when a custom reason phrase is required.

JSON response bodies are explicit raw strings at this layer. Structured JSON
serialization belongs at the call site or in `conflux.net.http.response_json`
helpers.

Typed cookie attributes can be built before appending a `Set-Cookie` header:

```cpp
auto response = http::text("ok");
response.set_cookie(
    http::cookie("session", id)
        .path("/")
        .http_only()
        .secure()
        .same_site(http::SameSite::Lax));
```

`http::cookie(...)` formats the attribute string for the existing
`Response::set_cookie(...)` storage path.

---

## SSE (Server-Sent Events)

### SSE route handler

```cpp
// SSE handlers are registered with router.sse(), not router.get().
router.sse("/events", [](RequestView const& req, std::shared_ptr<SseChannel> const& ch) {
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
    // Copies `frame`; the caller may release or mutate the source immediately.
    bool send_view(std::string_view frame);
    bool send_event(std::string_view type, std::string_view data);
    void on_close(std::function<void()> callback);
    void close();
};

class SseBroadcaster {
public:
    std::shared_ptr<SseChannel> subscribe();
    void broadcast(std::string frame);
};
```

`on_close` callbacks run once when the SSE channel closes. Middleware can attach
them to an SSE response returned through the facade path:

```cpp
app.use([](http::RequestView const& req, http::Next const& next) {
    auto response = next(req);
    if (response.is_sse()) {
        response.sse_channel_ptr()->on_close([] {
            record_stream_closed();
        });
    }
    return response;
});

app.use([](http::RequestView const& req,
           http::RequestContext const& ctx,
           http::AsyncNext const& next) -> http::Task<http::Response> {
    auto response = co_await next(req, ctx);
    response.headers["x-async-middleware"] = "1";
    co_return response;
});
```

---

## WebSocket

```cpp
app.ws("/ws", [](RequestView const& req, WsConn& ws) {
    while (auto frame = ws.recv()) {
        if (frame->opcode == WsConn::Opcode::Text) {
            ws.send_text(frame->payload);
        }
    }
});
```

`WsConn` provides blocking connection-local operations on the worker that owns the
upgraded connection. There is no server-owned WebSocket outbound backlog after
handoff: `send_text`, `send_binary`, and `send_ping` write the frame on the
owning worker thread under a connection-local send mutex. Slow peer backpressure
therefore blocks that WebSocket worker, not an HTTP ring thread. Applications
that need queued broadcast semantics should put an explicit queue in front of
`WsConn`, choose an `OverflowPolicy`, and account that queue in application
metrics.

```cpp
std::optional<WsConn::Frame> recv();
bool send_text(std::string_view);
bool send_binary(std::span<std::byte const>);
bool send_ping(std::string_view = {});
void on_close(std::function<void()> callback);
void close(uint16_t code = 1000, std::string_view reason = {});
```

WebSocket routing is implemented as a GET route that returns a `WsUpgrade`
response. The router handles the Upgrade/101 handshake transparently.
`on_close` callbacks run once when the connection closes through `close()` or
handler teardown.
`HttpPressureMetrics::websocket_closed_for_pressure` counts server-owned
handoff pressure, such as a full/closed WebSocket worker pool or a failed
blocking-fd transition before the handler owns the connection.

---

## Static/realtime component modules

`StaticOptions` is exported by `conflux.net.http.static_files` / `conflux::http_static`.
Server request vocabulary (`UploadedFile`, `Request`, `RequestView`,
`CloneableFunction`) is exported by `conflux.net.http.server_types`, which is
part of `conflux::http_core`. SSE and WebSocket types/helpers (`SseOverflowPolicy`,
`SseChannel`, `SseBroadcaster`, `WsConn`, `WsUpgrade`) are exported by
`conflux.net.http.realtime` / `conflux::http_realtime`. HTTP response
vocabulary (`Response`, `DeferredResponse`, mapped/streamed body carriers)
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

app.serve_static("/assets", "./public", StaticOptions{
    .precompressed = true,
    .cache_control = "max-age=86400, public",
});
```

Precompressed serving: if `Accept-Encoding: gzip` is present and `file.gz` exists next to `file`, the `.gz` sidecar is served directly with `Content-Encoding: gzip`. Same for `.br`. No runtime compression.

`allow_put` / `allow_delete` enable write operations on the served directory. Off by default.

`StreamedFile::on_complete(callback)` lets middleware observe streamed static
body completion where the response carries a `StreamedFile`. The callback runs
once with `StreamedFileResult::completed` after the file body is delivered, or
`StreamedFileResult::failed` if the stream is abandoned or the send path fails.

---

## Middleware

Password storage uses the dedicated `conflux.net.password_hash` boundary; see `docs/auth-password-hashing.md` for Argon2id/PBKDF2 formats and login-time rehash migration.

Middleware wraps every matched route. Applied outermost-first in registration order.

```cpp
router.use([](RequestView req, NextHandler next) -> Response {
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

### Built-in: observability

`http::observability()` is the app-facing facade for request IDs, W3C
`traceparent` propagation, structured access logs, and Prometheus text metrics:

```cpp
auto app = conflux::http::app();
app.use(conflux::http::observability({
    .service_name = "api",
    .metrics_path = "/metrics",
}));
```

The metrics route is registered by default and can be disabled with
`register_metrics_route = false`. `app.validate()` reports the normal duplicate
route issue if `metrics_path` collides with another `GET` route. Request metrics
use route patterns such as `/users/{id}` and `<unmatched>` for 404s; query
strings are never metric labels. Structured logs redact `Authorization`,
`Proxy-Authorization`, `Cookie`, `Set-Cookie`, `X-Api-Key`, `X-Api-Token`,
`X-Auth-Token`, and `X-CSRF-Token` by default.

Runtime pressure and work-pool metrics require explicit sources, for example
`ObservabilitySinks{.pressure_metrics = [&server] { return
server.metrics().pressure; }}` and `ObservabilityOptions::work_pools`. Streaming
responses currently measure response creation/header commit duration; stream
owners should export close-duration metrics separately if needed.
Parser/admission rejections are bridged into the same registry for servers
created through `App`; manual `Router` + `HttpServer` users can install
`observability_server_hooks()` explicitly.

---

## OpenAPI / Route metadata

Facade applications expose richer app metadata:

```cpp
auto routes = app.routes();          // AppRouteInfo: source, extractors, limits, auth, schemas
auto statics = app.static_mounts();  // AppStaticMountInfo: prefix, root, source location
auto table = app.route_table();      // printable startup/debug route table
auto spec = app.openapi_spec();      // app metadata backed OpenAPI JSON
```

`import conflux.http.extended;` also exposes `http::openapi_handler(app, title, version)`
for mounting the generated spec as a route handler, `http::use_async(app, mw)`
for codebases that prefer an explicit async-middleware spelling, plus
`http::router(app)` and `http::route_infos(app)` for integrations that
deliberately need the lower-level router metadata. The curated `App` surface
keeps `openapi_spec()` as plain data and uses `app.use(...)` as the single
middleware registration path.

`app.validate()` returns source locations for route and static-mount issues.
`ValidationReport::detailed_summary()` includes both the reported source and any
related source, such as the earlier registration for a duplicate route.
Stable diagnostic codes are included in summaries. For example:

```text
GET /same [http.route.duplicate]: duplicate route at src/app.cxx:42 related src/app.cxx:41
```

JSON extractor failures return problem JSON with grep-friendly codes. A type
mismatch decode failure is reported as:

```json
{"code":"json.decode.type_mismatch","stage":"decode","kind":"wrong_kind","detail":"expected object"}
```

The lower-level router still exposes minimal route metadata for advanced users:

```cpp
struct RouteInfo {
    std::string              method;
    std::string              path_pattern;  // OpenAPI style: /users/{id}
    std::vector<std::string> path_params;   // captured parameter names
};

auto infos = router.route_infos();
// Use with conflux.net.openapi to generate an OpenAPI spec document
```

`conflux.net.openapi` consumes router `route_infos()` and produces a minimal
OpenAPI 3.x JSON document. Import the narrow module directly when linking
`conflux::http_openapi`; the complete `conflux.net.http` umbrella also re-exports
it.

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

CPU pinning: set `ring_core` and `worker_core_base` in `Config`; benchmark context is tracked in [`../benchmarks/reproducibility.md`](../benchmarks/reproducibility.md).

---

## JSON route responses

First-contact application routes should return structured values with
`http::json(value)`:

```cpp
import conflux.http;

app.get("/api/count", [] {
    return http::json(Count{.value = 42});
});
```

Raw serialized JSON can still use `Response::json(std::string)` in lower-level
router code. Framework/reusable code that needs provider control should use
`http::codec::json`, which keeps the JSON boundary explicit instead of binding
route code to one concrete DOM:

```cpp
import conflux.net.http.response_json;

router.get("/api/count", [](RequestView const &) {
    return conflux::http::codec::json::response_or_internal_error_with<MyProvider>(
        static_cast<i64>(42));
});

router.post("/api/items", [](RequestView const &) {
    auto resp = conflux::http::codec::json::try_response_with<MyProvider>(
        ItemCreated{.id = 7},
        {.status = kHttpCreated, .status_text = "Created"});
    return resp ? std::move(*resp) : Response::internal_error();
});
```

For typed route registration, `conflux.net.http.app_json` keeps both request-body
decode and response serialization provider-explicit. Typed body helpers default
to `DecodeOptions{.copy_input = false}`, so providers that support direct struct
decode can read from the live request body without building an owning DOM. Pass
`DecodeOptions{.copy_input = true}` when a route intentionally needs the
provider's owning fallback.

```cpp
import conflux.net.http.app_json;

conflux::http::codec::json::routes<MyProvider>(app)
    .get("/api/count", [] { return Count{.value = 42}; })
    .post_body<CreateItem>("/api/items", [](CreateItem const& body) {
        return ItemCreated{.id = body.name};
    }, {.status = kHttpCreated, .status_text = "Created"});
```

Application code that intentionally uses the current native provider can import
`conflux.net.http.native_json` and use the default-provider overloads:

```cpp
import conflux.net.http.native_json;

router.get("/api/count", [](RequestView const &) {
    return conflux::http::codec::json::response_or_internal_error(static_cast<i64>(42));
});
```

`try_response_with` returns
`std::expected<Response, json::boundary::Error>` and preserves
provider-neutral serialization failures. `response_or_internal_error_with` is
the route helper for handlers that must return `Response`; it returns a
fixed JSON 500 body when serialization fails.

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
router.on_not_found([](http::RequestView const& req) -> http::Response {
    return http::Response::json(R"({"error":"not found"})", 404);
});

router.on_error([](http::RequestView const& req, std::exception const& ex) -> http::Response {
    // log ex, return 500
    return http::Response::internal_error(ex.what());
});
```
