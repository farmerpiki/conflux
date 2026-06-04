# Conflux HTTP Client — API & Contract

`conflux::http::HttpClient` — blocking HTTP/1.1 user agent. TLS via OpenSSL (`https://`) when the build has TLS support. Link the `conflux::http_client` component and import the first-contact `conflux.net.http.client` module; the component carries the lower-level client/wire modules plus runtime and DNS dependencies.

Also available: `async_send` — coroutine-based async transport backed by `SocketTaskRing`. HTTP and HTTPS (via `TcpTlsStream`). Happy Eyeballs (RFC 8305) staggered connect.

Status: **Blocking and async transports stable.** New connection per request (no pool, no keep-alive).

## Module imports

```cpp
import conflux.net.http.client;   // first-contact client surface
// or, granular:
import conflux.net.http.types;    // HttpError, HttpTimeouts, HttpTelemetry, Url
import conflux.net.http.request;  // ClientRequest, ClientRequest::Builder
import conflux.net.client;        // HttpClient, HttpClientOptions, ClientResponse, ClientResult
import conflux.net.async_client;  // async_send
```

All public types live in namespace `conflux::http`.

## Quick start

```cpp
using namespace conflux::http;

HttpClient client{};
auto result = client.blocking_send(
    ClientRequest::get("https://api.example.com/v1/ping")
        .accept_json()
        .bearer("eyJhbGc...")
        .timeouts({.first_byte = std::chrono::seconds{2}})
        .build());

if (!result) {
    std::println(std::cerr, "{}: {}", static_cast<int>(result.error().kind), result.error().message);
    return 1;
}
auto const &resp = *result;
std::println("{} {}", resp.head.status, resp.head.status_text);
std::println("ct={}", resp.head.headers["content-type"]);
std::println("body bytes={}", resp.body.size());
```

The `.build()` is optional — `Builder &&` converts implicitly to `ClientRequest`.

### Fallible request setup

Use the free `try_get` / `try_post` / `try_put` / `try_patch` / `try_del` / `try_head` helpers when URLs come from config or user input and setup should not throw. They parse the URL once, return the same fluent builder on success, and expose `UrlError` directly on failure.

```cpp
auto req_builder = try_get(configured_url);
if (!req_builder) {
    std::println(std::cerr, "bad URL: {}", req_builder.error().message);
    return 1;
}

auto req = std::move(*req_builder).accept_json().build();
```

For method strings computed at runtime, use `try_method(method, url)` for the builder or `try_request(method, url)` for the final request value. Existing `ClientRequest::get/post/...` factories keep the throwing behavior for literal URLs and compact examples.

## Public types

### `Url`

```cpp
struct Url {
    std::string scheme;          // "http" | "https" — lowercased
    std::string host;            // includes brackets for IPv6 ("[::1]")
    u16         port{80};        // defaults: 80 (http), 443 (https)
    std::string path{"/"};
    std::string query;           // raw, no leading '?'

    static std::expected<Url, UrlError> parse(std::string_view input);
    bool uses_default_port() const noexcept;
    std::size_t origin_form_target_size() const noexcept;
    void append_origin_form_target(std::string& out) const;
    std::size_t host_header_value_size(std::string_view override_host = {}) const noexcept;
    void append_host_header_value(std::string& out, std::string_view override_host = {}) const;
    std::string str() const;
    void set_query_param(std::string_view name, std::string_view value); // percent-encodes
};
```

Parse contract:
- Empty or > 8192 bytes → `UrlError`.
- Missing `://` or scheme not `http`/`https` → `UrlError`.
- IPv6 literals must be bracketed (`http://[::1]:8080/`).
- `port == 0` → `invalid_port`.
- Empty path defaults to `/`.

### `HttpFields`

Case-insensitive (when constructed with `true`) ordered field map. Used for both request headers and response headers. Vector-backed with linear lookup — `set()` collapses duplicates, `append()` keeps them, `values(name)` returns all.

```cpp
HttpFields h{true};
h.set("Accept", "application/json");
auto v = h["accept"];                 // "application/json", empty view if absent
```

### `HttpTimeouts`

```cpp
struct HttpTimeouts {
    std::chrono::milliseconds resolve       {5'000};
    std::chrono::milliseconds connect       {5'000};
    std::chrono::milliseconds tls           {5'000};
    std::chrono::milliseconds write         {30'000};
    std::chrono::milliseconds first_byte    {30'000};
    std::chrono::milliseconds between_bytes {30'000};
};
```

`<= 0` means indefinite. Resolution is whole seconds (ceiling). `resolve` is advisory for the blocking client when it falls back to `getaddrinfo`, which honors only the global resolver timeout, not this field.

### `HttpError` & `HttpErrorKind`

```cpp
enum class HttpErrorKind : u8 {
    dns, connect, tls, write, read, timeout,
    protocol, header_too_large, body_too_large,
    decompression, redirect_limit,
};

enum class HttpPhase : u8 {
    resolve, connect, tls, write, first_byte, between_bytes,
};

struct HttpError {
    HttpErrorKind kind{HttpErrorKind::protocol};
    HttpPhase     phase{};
    int           os_errno{0};      // populated for dns/connect/write/read
    int           tls_alert{0};     // OpenSSL ERR_GET_REASON when kind == tls
    std::string   verify_reason;    // X509 verify failure string when applicable
    std::string   message;
};
```

`decompression` is reserved for Phase 2 — Phase 1 does not auto-decode `Content-Encoding`. `redirect_limit` is used when redirect following exhausts `max_redirects`.

### Timeout and I/O classification

Use `HttpError::phase` to decide where a failure happened, and use
`HttpError::kind` to decide whether it was a deadline, OS I/O failure, protocol
failure, or TLS failure.

- Blocking client deadline expiry for response header wait returns
  `HttpErrorKind::timeout` with `phase == HttpPhase::first_byte` and
  `os_errno == 0`.
- Blocking client write/read syscall failures return `HttpErrorKind::write` or
  `HttpErrorKind::read` with the matching phase and `os_errno` from the failed
  operation.
- Async socket deadline/cancellation paths can surface as `HttpErrorKind::read`
  or `HttpErrorKind::write` with the matching phase and `os_errno == 0` when the
  underlying coroutine reports a deadline-style failure rather than an OS errno.
- `os_errno == 0` means the failure is framework-classified, timeout/deadline,
  cancellation, protocol, or TLS state. Do not treat it as errno success.

For retry policy, prefer phase-based decisions: retry connect/first-byte
timeouts only when the request is idempotent or explicitly replayable; retry
between-byte read failures only when the application can discard the partial
body and safely repeat the request.

### `HttpTelemetry`

```cpp
struct HttpTelemetry {
    std::chrono::nanoseconds dns, connect, tls, ttfb, body;
    std::optional<std::chrono::nanoseconds> pool_wait;   // Phase 2

    u64  bytes_sent, bytes_received;                     // wire bytes incl. headers
    bool reused_connection{false};                       // Phase 2 — always false now
    std::string negotiated_protocol;                     // "https/1.1" or empty
    std::string tls_cipher, tls_version;
    bool tls_verified{false};                            // true iff verify_peer && handshake passed
    std::string peer_addr;                               // "ip:port" or "[ip6]:port"
    std::optional<std::string> decoded_encoding;         // Phase 2
};
```

Always populated on success. On failure the partial telemetry is **not** returned — only `HttpError`.

### `ClientResponse`

```cpp
struct ClientResponseHead {
    int                       status{502};
    std::string               status_text{"Bad Gateway"};
    HttpFields                headers{true};         // case-insensitive
    std::vector<std::string>  set_cookies;            // raw Set-Cookie values, in order
};

struct ClientResponse {
    ClientResponseHead head;
    std::string      body;           // already de-chunked / fully assembled
    HttpTelemetry    telemetry;
};

using ClientResult = std::expected<ClientResponse, HttpError>;

using ClientBodyChunkSink = std::function<bool(std::string_view)>;

struct ClientStreamResponse {
    ClientResponseHead head;
    HttpTelemetry      telemetry;
};

using ClientStreamResult = std::expected<ClientStreamResponse, HttpError>;
```

Header rules on the response:
- All hop-by-hop headers (`connection`, `keep-alive`, `te`, `trailers`, `transfer-encoding`, `upgrade`, `proxy-authenticate`, `proxy-authorization`) are **stripped** before being placed in `headers`.
- `Set-Cookie` values are **never** in `headers` — they are split into `set_cookies` in arrival order.
- `Content-Length` and `Transfer-Encoding` are consumed by the transport and are not surfaced in `headers`.
- `body` is fully decoded: chunked transfer is de-chunked, `Content-Length` body is read to length, EOF-delimited body is read to socket close.

Body for `HEAD` is always empty even if the server sends one.

## `ClientRequest`

Immutable value object. Construct via `ClientRequest::<verb>(url)` factories that return a `Builder`.

```cpp
class ClientRequest {
public:
    static Builder get   (std::string_view url);
    static Builder post  (std::string_view url);
    static Builder put   (std::string_view url);
    static Builder patch (std::string_view url);
    static Builder del   (std::string_view url);
    static Builder head  (std::string_view url);
    static Builder method(std::string_view m, std::string_view url);

    // Fallible free-function equivalents in namespace conflux::http:
    // try_get/try_post/try_put/try_patch/try_del/try_head/try_method/try_request

    std::string_view method()    const noexcept;
    Url const &      url()       const noexcept;
    HttpFields const &headers()  const noexcept;
    std::string const &body()    const noexcept;
    HttpTimeouts     timeouts()  const noexcept;
    bool             verify_peer() const noexcept;
    std::string_view server_name() const noexcept;
    int              max_redirects() const noexcept;
};
```

The factories **throw `std::invalid_argument`** if the URL fails to parse. If you want fallible construction, parse `Url::parse` yourself first and pass the `Url` via `.url(Url)`.

### `ClientRequest::Builder`

Chained, supports both lvalue (`Builder &`) and rvalue (`Builder &&`) overloads — chaining off a temporary works without dangling. Implicitly convertible to `ClientRequest` on rvalue, or call `.build() &&`.

URL / verb:
```cpp
.method(sv) .url(sv) .url(Url)
.query(name, value)
.query_params(HttpFields const &)
```

Headers:
```cpp
.header(name, value)
.headers(HttpFields)            // merges, last-wins per name
.bearer(token)                  // Authorization: Bearer <token>
.basic(user, pass)              // Authorization: Basic <base64>
.user_agent(sv) .accept(sv) .accept_json()
.content_type(sv)
.if_match(etag) .if_none_match(etag)
.if_modified_since(time_point) .if_unmodified_since(time_point)
```

Body (debug builds assert that body is set at most once unless `clear_body()` is called between):
```cpp
.body(std::string)              // takes ownership
.body_view(std::string_view)    // copies into request
.body_form(HttpFields)          // sets Content-Type: application/x-www-form-urlencoded
.body_json_raw(std::string)     // sets Content-Type: application/json
.clear_body()
```

JSON request-body serialization lives in optional HTTP JSON modules instead of
`ClientRequest::Builder`, so `conflux.net.http.request` stays free of a JSON
module dependency. Reusable/framework code should use the provider-explicit
`conflux.net.http.json` helpers; app code can import the native convenience edge
when it intentionally chooses the current `conflux.json` adapter:

```cpp
import conflux.net.http.json;

auto b = ClientRequest::post("https://example.test/submit");
conflux::http::codec::json::set_body_with<CustomProvider>(b, value);
auto req = std::move(b).build();
```

```cpp
import conflux.net.http.native_json;

auto b = ClientRequest::post("https://example.test/submit");
conflux::http::codec::json::set_body(b, doc);
auto req = std::move(b).build();
```

Execution policy:
```cpp
.timeouts(HttpTimeouts)
.follow_redirects(int max = 10) .disable_redirects()
.verify_peer(bool)
.server_name(sv)                 // SNI override; defaults to URL host
```

## `HttpClient`

```cpp
struct HttpClientOptions {
    HttpTimeouts default_timeouts;
    bool         verify_peer{true};
    std::string  ca_bundle_path;                 // empty → OpenSSL default paths
    std::size_t  max_header_bytes   {64 * 1024};
    std::size_t  max_body_bytes     {16 * 1024 * 1024};
    std::size_t  max_buffered_bytes {4 * 1024 * 1024}; // caps transient dechunk buffering
    HttpFields   default_headers;                // merged into every request, request wins
    void*        resolver{nullptr};              // conflux::net::dns::Resolver*, optional
};

class HttpClient {
public:
    explicit HttpClient(HttpClientOptions opts = {});
    HttpClientOptions const &options() const noexcept;
    ClientResult blocking_send(ClientRequest const& req) const;
    ClientStreamResult blocking_send_streaming(
        ClientRequest const& req,
        ClientBodyChunkSink sink) const;
};
```

`max_buffered_bytes` limits transient buffering while dechunking responses in
the blocking and async clients. It is not a streaming or backpressure API; the
response body is still materialized subject to `max_body_bytes`.

### `blocking_send` contract

Sequence:
1. **DNS** — uses `opts.resolver` (`conflux::net::dns::Resolver*`) when provided; otherwise uses `getaddrinfo(AF_UNSPEC, SOCK_STREAM)`. Tries every address until one connects.
2. **Connect** — non-blocking + `poll(POLLOUT)` honoring `timeouts.connect`. Records `peer_addr` in telemetry.
3. **TLS handshake** (https only) — `SSL_connect` honoring `timeouts.tls`. SNI from `.server_name()` else URL host. Hostname verification on when `verify_peer && opts.verify_peer`. Builds a fresh `TlsContext` per request — no session cache.
4. **Send** — request line + merged headers (`opts.default_headers` first, then request, request wins). Skips any caller-supplied hop-by-hop headers and any `Host` (regenerated from URL unless caller explicitly set one — that's what `proxy.cxx` uses for `preserve_host`). Always emits `Connection: close`, plus `Content-Length` if `body` is non-empty. No `Transfer-Encoding: chunked` is ever sent — chunked request bodies are out of scope for Phase 1.
5. **Receive headers** — read until `\r\n\r\n`, capped at `opts.max_header_bytes + 4096`. Records `ttfb`.
6. **Parse status + headers** — drops hop-by-hop, separates `Set-Cookie`, captures `Content-Length` / `Transfer-Encoding`.
7. **Receive body** — chunked → de-chunk; `Content-Length` → read exact; otherwise → read to EOF. Capped at `opts.max_body_bytes` regardless of mode.
8. **Close** the connection — Phase 1 never reuses sockets.

Method-specific:
- `HEAD` skips body recv; final `body` is empty.

Use `blocking_send(...)` for caller-thread socket/poll/TLS I/O.

### `blocking_send_streaming` contract

`blocking_send_streaming(req, sink)` uses the same blocking HTTP/1.1 transport
but sends de-chunked body bytes to `sink(std::string_view)` as they arrive
instead of storing them in `ClientResponse::body`.

```cpp
HttpClient client{};
std::string rendered;

auto response = client.blocking_send_streaming(
    ClientRequest::get("http://127.0.0.1:8080/events").build(),
    [&](std::string_view chunk) {
        rendered.append(chunk);
        return true;
    });
```

The sink view is valid only for the callback duration. Return `true` to keep
reading. Returning `false` stops the transfer and reports a `HttpErrorKind::read`
failure with `phase == HttpPhase::between_bytes`.

The method still enforces `max_header_bytes`, `max_body_bytes`, and
`max_buffered_bytes`. For `Transfer-Encoding: chunked`, chunks passed to the sink
are decoded body bytes, not wire chunk frames. For `HEAD`, the sink is not called.

Automatic redirect following is intentionally not part of this streaming entry
point yet; redirect bodies would otherwise be indistinguishable from the final
response body at the sink. Callers that need redirects should resolve them before
opening a streaming response.

### Error mapping

| Failure | `kind` | `phase` | populated |
|---|---|---|---|
| configured resolver / `getaddrinfo` returns an error or no addresses | `dns` | `resolve` | — |
| All addresses fail to `connect` | `connect` | `connect` | `os_errno` |
| TLS context construction fails | `tls` | `tls` | `message` |
| TLS handshake fails | `tls` | `tls` | `tls_alert`, `verify_reason` (when verify failed) |
| `send` fails | `write` | `write` | `os_errno` |
| Headers exceed cap or no `\r\n\r\n` arrived | `header_too_large` / `protocol` | — | `message` |
| Status line malformed / non-numeric / out of `[100,999]` | `protocol` | — | `message` |
| Body exceeds `max_body_bytes` (any mode) | `body_too_large` | — | `message` |
| Chunked decode invalid or `recv` failed mid-body | `read` (or `protocol`) | `between_bytes` | `os_errno` |

A `poll` timeout currently surfaces as `read` / `write` (whichever phase was waiting), with `os_errno == 0`. There is no distinct `timeout` kind in Phase 1 even though the enum reserves one.

## Chunked decoding

Standalone chunked-body parsing now lives in `conflux.net.http_server_helpers`.
Import that module and call `conflux::http::decode_chunked(...)` or `conflux::http::decode_chunked_incremental(...)` when you need to process already-received chunked data outside the client transport.

## What this client does NOT do (yet)

Anything that depends on Phase 2:
- **No connection pool** — each call opens & closes a socket. `pool_wait` / `reused_connection` are wired but never set.
- **No keep-alive** — request always emits `Connection: close`.
  Future pooling belongs in the client transport, keyed by scheme, host, port, TLS configuration, and proxy configuration. It must define keep-alive eligibility, maximum idle connections, deadline/cancellation behavior, HTTP/1.1 reuse safety, and an HTTP/2 multiplexing path before proxy code can benefit from it.
- **No HTTP/2 or HTTP/3** — server-side modules exist but the client speaks HTTP/1.1 only. `negotiated_protocol` is hard-coded to `"https/1.1"` for TLS responses.
- **No content-coding decode** — `gzip`/`br`/`zstd` bodies arrive un-inflated. Caller must decode (the `conflux.net.compress` module is server-side).
- **No request streaming / chunked send** — body is buffered in full; no `Transfer-Encoding` on the wire.
- **No proxy support on the client itself** — `src/net/proxy.cxx` is a *server-side* reverse-proxy handler that uses `HttpClient` internally; it is not an HTTP-proxy client.
- **No cookie jar** — `Set-Cookie` is captured in `head.set_cookies` for inspection only.
- **No retry / backoff.**

## Threading

`HttpClient` is cheap, copyable, and stateless (apart from `HttpClientOptions`).
`blocking_send` and `blocking_send_streaming` are reentrant — call from any
thread. There is no shared state between concurrent calls. The blocking
transport waits on the caller thread for `poll`/socket/TLS I/O.

## TLS contract

- HTTPS support is enabled only when `CONFLUX_HAS_TLS=1` for the client target. In builds without TLS support, the HTTP client still builds and plain `http://` requests work, but any `https://` request fails with `kind == tls`, `message == "TLS not available (built without TLS)"`.
- `verify_peer == true` is the default. Verification uses either `opts.ca_bundle_path` or OpenSSL's default trust store.
- SNI and hostname verification both target `request.server_name()` if set, else `url.host`. To talk to a server whose certificate names a host that differs from the IP you're dialing, set both `.url("https://10.0.0.5/...")` and `.server_name("api.internal")`.
- `tls_cipher` / `tls_version` / `tls_verified` in telemetry reflect the established session.

## Limits & defaults summary

| Knob | Default | Hard cap |
|---|---|---|
| `max_header_bytes` | 64 KiB | option-controlled |
| `max_body_bytes` | 16 MiB | option-controlled |
| URL length | — | 8192 bytes (`Url::parse`) |
| receive buffer per `recv` | 4096 | hard-coded |
| connect/tls/resolve timeout | 5 s | option-controlled |
| write/first-byte/between-bytes timeout | 30 s | option-controlled |

## Async client (`async_send`)

**Module:** `conflux.net.async_client` — not re-exported from `conflux.net.http`.

```cpp
import conflux.net.async_client;

[[nodiscard]] conflux::work::root::Task<ClientResult>
conflux::http::async_send(
    HttpClient const& client,
    SocketTaskRing&   ring,
    ClientRequest const& req);
```

Runs on the caller's `SocketTaskRing`. The `client`, `ring`, and `req` must all
outlive the coroutine; do not destroy them while the task is suspended.
Cancellation is routed through `SocketTaskRing` and is best effort for
already-submitted DNS, connect, TLS, write, and read work. Once `async_send`
has produced a terminal `ClientResult` or error, later cancellation does not
rewrite that result.

**Features:**

- HTTP and HTTPS (async TLS via `TcpTlsStream` with memory BIOs, deadline-aware handshake/read/write)
- Happy Eyeballs (RFC 8305 v1): `staggered_parallel_connect` with 250 ms stagger, 10 ms fast-fail poll
- Write timeout: `submit_send_timeout_borrowed` (linked SQE + timeout)
- Cancellation-safe close: `CloseState` shields close SQE from outer cancel

**Limitations vs `blocking_send`:**

| Feature | `blocking_send` | `async_send` |
|---|---|---|
| HTTP/1.1 | Yes | Yes |
| HTTPS / TLS | Yes | Yes (via `TcpTlsStream`) |
| Happy Eyeballs | No (sequential) | Yes (RFC 8305 stagger) |
| Connection pool | No | No |
| Redirect following | Yes | Yes |
| Content-encoding decode | No | No |
| Write timeout | Yes | Yes (linked SQE) |
| Cancellation | N/A | Via `SocketTaskRing` cancel |

Error kinds, `ClientResult`, and `ClientResponse` shapes are identical to `blocking_send`.

### Server context-route dispatch and client calls

Context routes are server-side. They use `Request` / `Response` and live
on the server import surface. Use this shape when a route needs the ring context
to call `async_send` without blocking the HTTP ring thread.

```cpp
import conflux.http;
import conflux.net.async_client;

using ContextHandler =
    CloneableFunction<root::Task<Response>(RequestView const&, RequestContext const&)>;

Router& Router::get_context(std::string_view path, F&& handler);
std::optional<Response>
Router::dispatch_context(RequestView const&, RequestContext const&);
```

Use `dispatch_context(...)` because the function returns an optional response immediately and
represents context/deferred route probing, not an awaitable async operation.

## Stability

Blocking and async surfaces stable. Both support HTTP and HTTPS.

Still not in any transport:
- pooled connections / keep-alive (`pool_wait` / `reused_connection` always false)
- content-coding decode (gzip/br/zstd bodies arrive raw)
- JSON document request-body helpers are free functions in `conflux.net.http.json`; there is intentionally no `body_json(NodeRef)` member on `ClientRequest::Builder`. The helpers are provider-boundary based, so framework-facing code should not call `Document::dump()` directly.

Builder/Request/Response/Telemetry/Error shapes are not expected to change.
