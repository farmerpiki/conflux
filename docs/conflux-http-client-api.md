# Conflux HTTP Client — API & Contract

`conflux::http::HttpClient` — blocking HTTP/1.1 user agent. TLS via OpenSSL (`https://`). One module, no extra deps.

Also available: `send_async` — coroutine-based async transport backed by `SocketTaskRing`. HTTP and HTTPS (via `TcpTlsStream`). Happy Eyeballs (RFC 8305) staggered connect.

Status: **Blocking and async transports stable.** New connection per request (no pool, no keep-alive).

## Module imports

```cpp
import conflux.net.http;          // umbrella — exports everything below
// or, granular:
import conflux.net.http.types;    // HttpError, HttpTimeouts, HttpTelemetry, Url
import conflux.net.http.request;  // HttpRequest, HttpRequest::Builder
import conflux.net.client;        // HttpClient, HttpClientOptions, HttpResponse, HttpResult
import conflux.net.async_client;  // send_async — NOT re-exported from conflux.net.http
```

All public types live in namespace `conflux::http`.

## Quick start

```cpp
using namespace conflux::http;

HttpClient client{};
auto result = client.send_blocking(
    HttpRequest::get("https://api.example.com/v1/ping")
        .accept_json()
        .bearer("eyJhbGc...")
        .timeouts({.first_byte = std::chrono::seconds{2}})
        .build());

if (!result) {
    std::println(stderr, "{}: {}", static_cast<int>(result.error().kind), result.error().message);
    return 1;
}
auto const &resp = *result;
std::println("{} {}", resp.head.status, resp.head.status_text);
std::println("ct={}", resp.head.headers["content-type"]);
std::println("body bytes={}", resp.body.size());
```

The `.build()` is optional — `Builder &&` converts implicitly to `HttpRequest`.

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

Case-insensitive (when constructed with `true`) ordered field map. Used for both request headers and response headers. Multimap-backed — `set()` collapses duplicates, `append()` keeps them, `values(name)` returns all.

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

`<= 0` means indefinite. Resolution is whole seconds (ceiling). `resolve` is currently advisory — DNS uses `getaddrinfo`, which honors only the global resolver timeout, not this field.

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

`decompression` and `redirect_limit` are reserved for Phase 2 — Phase 1 does not auto-decode `Content-Encoding`, but redirect following is implemented and driven by `max_redirects`.

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

### `HttpResponse`

```cpp
struct HttpResponseHead {
    int                       status{502};
    std::string               status_text{"Bad Gateway"};
    HttpFields                headers{true};         // case-insensitive
    std::vector<std::string>  set_cookies;            // raw Set-Cookie values, in order
};

struct HttpResponse {
    HttpResponseHead head;
    std::string      body;           // already de-chunked / fully assembled
    HttpTelemetry    telemetry;
};

using HttpResult = std::expected<HttpResponse, HttpError>;
```

Header rules on the response:
- All hop-by-hop headers (`connection`, `keep-alive`, `te`, `trailers`, `transfer-encoding`, `upgrade`, `proxy-authenticate`, `proxy-authorization`) are **stripped** before being placed in `headers`.
- `Set-Cookie` values are **never** in `headers` — they are split into `set_cookies` in arrival order.
- `Content-Length` and `Transfer-Encoding` are consumed by the transport and are not surfaced in `headers`.
- `body` is fully decoded: chunked transfer is de-chunked, `Content-Length` body is read to length, EOF-delimited body is read to socket close.

Body for `HEAD` is always empty even if the server sends one.

## `HttpRequest`

Immutable value object. Construct via `HttpRequest::<verb>(url)` factories that return a `Builder`.

```cpp
class HttpRequest {
public:
    static Builder get   (std::string_view url);
    static Builder post  (std::string_view url);
    static Builder put   (std::string_view url);
    static Builder patch (std::string_view url);
    static Builder del   (std::string_view url);
    static Builder head  (std::string_view url);
    static Builder method(std::string_view m, std::string_view url);

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

### `HttpRequest::Builder`

Chained, supports both lvalue (`Builder &`) and rvalue (`Builder &&`) overloads — chaining off a temporary works without dangling. Implicitly convertible to `HttpRequest` on rvalue, or call `.build() &&`.

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

JSON request-body serialization lives in the optional `conflux.net.http.json`
module instead of `HttpRequest::Builder`, so `conflux.net.http.request` stays
free of a JSON module dependency:

```cpp
import conflux.net.http.json;

auto b = HttpRequest::post("https://example.test/submit");
conflux::http::json::set_body(b, doc);
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
    std::size_t  max_buffered_bytes {4 * 1024 * 1024}; // Phase 2 — unused now
    HttpFields   default_headers;                // merged into every request, request wins
};

class HttpClient {
public:
    explicit HttpClient(HttpClientOptions opts = {});
    HttpClientOptions const &options() const noexcept;
    HttpResult send_blocking(HttpRequest req) const;
};
```

### `send_blocking` contract

Sequence:
1. **DNS** — `getaddrinfo(AF_UNSPEC, SOCK_STREAM)`. Tries every address until one connects.
2. **Connect** — non-blocking + `poll(POLLOUT)` honoring `timeouts.connect`. Records `peer_addr` in telemetry.
3. **TLS handshake** (https only) — `SSL_connect` honoring `timeouts.tls`. SNI from `.server_name()` else URL host. Hostname verification on when `verify_peer && opts.verify_peer`. Builds a fresh `TlsContext` per request — no session cache.
4. **Send** — request line + merged headers (`opts.default_headers` first, then request, request wins). Skips any caller-supplied hop-by-hop headers and any `Host` (regenerated from URL unless caller explicitly set one — that's what `proxy.cxx` uses for `preserve_host`). Always emits `Connection: close`, plus `Content-Length` if `body` is non-empty. No `Transfer-Encoding: chunked` is ever sent — chunked request bodies are out of scope for Phase 1.
5. **Receive headers** — read until `\r\n\r\n`, capped at `opts.max_header_bytes + 4096`. Records `ttfb`.
6. **Parse status + headers** — drops hop-by-hop, separates `Set-Cookie`, captures `Content-Length` / `Transfer-Encoding`.
7. **Receive body** — chunked → de-chunk; `Content-Length` → read exact; otherwise → read to EOF. Capped at `opts.max_body_bytes` regardless of mode.
8. **Close** the connection — Phase 1 never reuses sockets.

Method-specific:
- `HEAD` skips body recv; final `body` is empty.

### Error mapping

| Failure | `kind` | `phase` | populated |
|---|---|---|---|
| `getaddrinfo` returns nonzero or no addresses | `dns` | `resolve` | — |
| All addresses fail to `connect` | `connect` | `connect` | `os_errno` |
| TLS context construction fails | `tls` | `tls` | `message` |
| TLS handshake fails | `tls` | `tls` | `tls_alert`, `verify_reason` (when verify failed) |
| `send` fails | `write` | `write` | `os_errno` |
| Headers exceed cap or no `\r\n\r\n` arrived | `header_too_large` / `protocol` | — | `message` |
| Status line malformed / non-numeric / out of `[100,999]` | `protocol` | — | `message` |
| Body exceeds `max_body_bytes` (any mode) | `body_too_large` | — | `message` |
| Chunked decode invalid or `recv` failed mid-body | `read` (or `protocol`) | `between_bytes` | `os_errno` |

A `poll` timeout currently surfaces as `read` / `write` (whichever phase was waiting), with `os_errno == 0`. There is no distinct `timeout` kind in Phase 1 even though the enum reserves one.

## Free functions

```cpp
[[nodiscard]] std::optional<std::string>
conflux::http::decode_chunked_body(std::string_view encoded);
```

Pure-function chunked decoder for callers that have already received an entire chunked body. Returns `nullopt` if encoding is invalid, incomplete, or has trailing bytes after the final chunk.

## What this client does NOT do (yet)

Anything that depends on Phase 2:
- **No connection pool** — each call opens & closes a socket. `pool_wait` / `reused_connection` are wired but never set.
- **No keep-alive** — request always emits `Connection: close`.
- **No HTTP/2 or HTTP/3** — server-side modules exist but the client speaks HTTP/1.1 only. `negotiated_protocol` is hard-coded to `"https/1.1"` for TLS responses.
- **Automatic redirect following** — `follow_redirects` / `disable_redirects` control the maximum number of redirects followed by the client; redirects strip sensitive headers on host changes and report `redirect_limit` when exhausted.
- **No content-coding decode** — `gzip`/`br`/`zstd` bodies arrive un-inflated. Caller must decode (the `conflux.net.compress` module is server-side).
- **No request streaming / chunked send** — body is buffered in full; no `Transfer-Encoding` on the wire.
- **No proxy support on the client itself** — `src/net/proxy.cxx` is a *server-side* reverse-proxy handler that uses `HttpClient` internally; it is not an HTTP-proxy client.
- **No cookie jar** — `Set-Cookie` is captured in `head.set_cookies` for inspection only.
- **No retry / backoff.**

## Threading

`HttpClient` is cheap, copyable, and stateless (apart from `HttpClientOptions`). `send_blocking` is reentrant — call from any thread. There is no shared state between concurrent calls. `do_blocking_request` blocks the caller thread on `poll`/socket I/O.

## TLS contract

- Built only when `CONFLUX_HAS_TLS` is set at compile time. Otherwise any `https://` request fails with `kind == tls`, `message == "TLS not available (built without TLS)"`.
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

## Async client (`send_async`)

**Module:** `conflux.net.async_client` — not re-exported from `conflux.net.http`.

```cpp
import conflux.net.async_client;

[[nodiscard]] conflux::work::root::Task<HttpResult>
conflux::http::send_async(
    HttpClient const& client,
    SocketTaskRing&   ring,
    HttpRequest const& req);
```

Runs on the caller's `SocketTaskRing`. The `client`, `ring`, and `req` must all outlive the coroutine — do not destroy them while the task is suspended.

**Features:**

- HTTP and HTTPS (async TLS via `TcpTlsStream` with memory BIOs, deadline-aware handshake/read/write)
- Happy Eyeballs (RFC 8305 v1): `staggered_parallel_connect` with 250 ms stagger, 10 ms fast-fail poll
- Write timeout: `submit_send_timeout_borrowed` (linked SQE + timeout)
- Cancellation-safe close: `CloseState` shields close SQE from outer cancel

**Limitations vs `send_blocking`:**

| Feature | `send_blocking` | `send_async` |
|---|---|---|
| HTTP/1.1 | Yes | Yes |
| HTTPS / TLS | Yes | Yes (via `TcpTlsStream`) |
| Happy Eyeballs | No (sequential) | Yes (RFC 8305 stagger) |
| Connection pool | No | No |
| Redirect following | No | No |
| Content-encoding decode | No | No |
| Write timeout | Yes | Yes (linked SQE) |
| Cancellation | N/A | Via `SocketTaskRing` cancel |

Error kinds, `HttpResult`, and `HttpResponse` shapes are identical to `send_blocking`.

### Router async dispatch (`ContextHandler`)

**Module:** `conflux.net.http` (umbrella)

```cpp
struct RequestContext {
    SocketTaskRing& ring;
};

using ContextHandler   = std::function<root::Task<HttpResponse>(HttpRequestView, RequestContext)>;
using ContextMiddleware = std::function<root::Task<HttpResponse>(HttpRequestView, RequestContext, NextContextHandler)>;

Router& Router::add_context(std::string_view method, std::string_view path, ContextHandler);
bool    Router::has_context_routes() const;
root::Task<HttpResponse> Router::dispatch_async(HttpRequestView, RequestContext);
```

Context routes are dispatched via `try_dispatch_async` in the server's ring loop — they receive the ring thread's `SocketTaskRing` and can call `send_async` without blocking. `proxy_context_handler()` in `proxy.cxx` uses this path.

## Stability

Blocking and async surfaces stable. Both support HTTP and HTTPS.

Still not in any transport:
- pooled connections / keep-alive (`pool_wait` / `reused_connection` always false)
- redirect following (`follow_redirects` follows redirects up to the configured limit)
- content-coding decode (gzip/br/zstd bodies arrive raw)
- JSON document request-body helpers are free functions in `conflux.net.http.json`; there is intentionally no `body_json(NodeRef)` member on `HttpRequest::Builder`.

Builder/Request/Response/Telemetry/Error shapes are not expected to change.
