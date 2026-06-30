# Streaming Upload Application API Plan

Status: raw `UploadBody` implementation landed; remaining TODO is deferred
multipart streaming and any HTTP/3 parity gaps.
Branch: `feature/http-upload-streaming-api`
Expected follow-up lane: split `MultipartUpload` into a new implementation plan
only when a caller needs streaming multipart. Keep HTTP/3 parity as a separate
transport conformance check.

## Summary

Raw `http::UploadBody` landed as an ergonomic typed extractor aligned with
existing `App` extractors. The landed surface includes route body-mode metadata,
bounded queue rejection for unread uploads, explicit read/error/early-return
semantics, HTTP/1 and HTTP/2 transport support, and `save_to()` using explicit
async file I/O. `http::MultipartUpload` remains deferred as the streaming
counterpart to buffered `Multipart`.

## Public API

- Landed: move-only async-only `http::UploadBody`, the streaming counterpart to
  `BodyBytes`:

```cpp
app.post("/upload", [](http::UploadBody body) -> conflux::work::Task<http::Response> {
    std::uint64_t total = 0;
    while (true) {
        auto read = co_await body.read();
        if (!read) {
            co_return http::upload_error_response(read.error());
        }
        if (!*read) {
            break;
        }
        total += (*read)->bytes().size();
    }
    co_return http::text(std::format("bytes={}", total));
}).max_body_size(10 * 1024 * 1024);
```

- Use error-aware reads:

```cpp
using UploadReadResult = std::expected<std::optional<UploadChunk>, UploadError>;
[[nodiscard]] Task<UploadReadResult> UploadBody::read();
```

- `std::nullopt` means EOF only; malformed framing, disconnect, timeout, cancellation, content-length mismatch, body-limit failure, and multipart parse failure return `UploadError`.
- After `read()` returns `UploadError`, all later reads return the same terminal error or `cancelled`; no further chunks are produced.
- Define:

```cpp
enum class UploadErrorKind {
    malformed_body,
    disconnected,
    timeout,
    cancelled,
    body_too_large,
    content_length_mismatch,
    io_error,
};

struct UploadError {
    UploadErrorKind kind{};
    std::string detail{};
};

[[nodiscard]] std::string_view upload_error_code(UploadErrorKind kind) noexcept;
[[nodiscard]] Response upload_error_response(UploadError const &error);

struct UploadSaveOptions {
    bool overwrite{};
    bool create_parent_dirs{};
    std::optional<std::uint64_t> max_bytes{};
    std::size_t buffer_size = 128 * 1024;
};

struct UploadSaveResult {
    std::filesystem::path path;
    std::uint64_t bytes_written{};
};
```

- `upload_error_response()` maps upload failures to protocol-appropriate responses, including `body_too_large -> 413`, `timeout -> 408`, malformed body/content-length mismatch -> `400`, and disconnect/cancel paths that close or mark the connection non-reusable.
- `UploadChunk` mirrors existing extractor wrappers:

```cpp
struct UploadChunk {
    using value_type = std::span<std::byte const>;

    value_type value{};

    [[nodiscard]] constexpr value_type get() const noexcept { return value; }
    [[nodiscard]] constexpr value_type operator*() const noexcept { return value; }
    [[nodiscard]] constexpr value_type bytes() const noexcept { return value; }
    [[nodiscard]] std::string_view text_view() const noexcept;
};
```

- `UploadBody` also exposes `content_length()`, `bytes_read()`, `[[nodiscard]] discard() -> Task<std::expected<void, UploadError>>`, and `[[nodiscard]] save_to(...) -> Task<std::expected<UploadSaveResult, UploadError>>`.
- `save_to()` must not perform hidden blocking disk I/O on the ring thread. It uses existing async file I/O where available, or a clearly named explicit offload path. On error/cancel, incomplete destination files are removed unless options later add a deliberate keep-partial mode. Successful `save_to()` does not fsync and makes no durability guarantee beyond successful write/close; a future option may add fsync.
- `save_to()` writes exactly the supplied path. It does not sanitize multipart filenames or other user input; applications must choose and canonicalize destination paths.
- Deferred follow-up: add move-only async-only `http::MultipartUpload`, the streaming counterpart to `Multipart`.
- `MultipartUpload::read()` returns:

```cpp
using MultipartUploadReadResult = std::expected<std::optional<MultipartUploadEvent>, UploadError>;
[[nodiscard]] Task<MultipartUploadReadResult> MultipartUpload::read();
```

- `MultipartUploadEvent` has `PartBegin`, `Data`, and `PartEnd` alternatives. `PartBegin::headers` uses `HttpFieldsView`; part metadata and header views are borrowed until the next `read()` unless explicitly copied.
- Keep existing buffered `BodyText`, `BodyBytes`, `OwnedBodyBytes`, and `Multipart` behavior unchanged.

## Semantics

- Handler reads to EOF: request can remain eligible for keep-alive.
- Handler calls `co_await body.discard()`: server drains safely, then request can remain eligible for keep-alive.
- Handler returns without EOF or `discard()`: remaining upload is canceled and the connection is not reused.
- Transport cancellation, early response, or connection teardown while a read is pending completes the waiter with cancellation/disconnect. `UploadBody` destruction itself does not own transport cancellation; the transport-owned upload state does.
- Peer disconnect returns `UploadErrorKind::disconnected` on the next read.
- `discard()` after EOF returns success; after a terminal read error it returns the same error; after handler cancellation/destruction it completes as cancelled.
- Concurrent reads are invalid. A second pending `UploadBody::read()` returns `UploadErrorKind::cancelled` with detail `concurrent UploadBody::read() is not allowed`.
- `Expect: 100-continue`: match route and run auth, rate-limit, and known `Content-Length` limit checks before sending `100 Continue`; rejection sends only the final status.

## Landed Implementation Shape

- `BodyMode { none, buffered_raw, buffered_multipart, streaming_raw, streaming_multipart }` exists in route metadata. Validation rejects mixed buffered/streaming body extractors, streaming extractors in sync handlers, and streaming body on GET unless explicitly allowed.
- Known length: reject `Content-Length > limit` before handler start. Unknown/chunked: start handler after prelude and enforce cumulative bytes. Multipart: enforce total body limit plus bounded part-header limits. `UploadBody` defaults OpenAPI `consumes` to `application/octet-stream`; `MultipartUpload` defaults to `multipart/form-data`.
- Router lookup for headers-only route selection lets transports identify streaming routes before buffering the body.
- The internal upload stream is bounded, single-consumer, and uses `TaskSource` for waiters, with EOF, error, cancellation, disconnect, and queue-full rejection states.
- Upload stream queue capacity comes from `Config` upload defaults. Queue overflow is rejected and counted rather than silently buffering or blocking the ring.
- HTTP/1 and HTTP/2 are covered for raw `UploadBody`. HTTP/3 keeps a protocol-neutral public API, but H3 conformance remains a separate acceptance gate if local H3 async/context dispatch is not mature enough.
- Upload metrics/rejections cover streams started, bytes received, bytes consumed, backpressure events, canceled by handler, disconnected, body too large, and content-length mismatch. Multipart parse errors remain part of the deferred `MultipartUpload` lane. Collapse transport-internal flow-control failures to `io_error`; the raw landing does not expose a public flow-control error kind.
- `MultipartUpload` design keeps `MultipartUpload` as the name. Part names, filenames, content types, and headers are borrowed until the next `read()` unless explicitly copied; part headers have separate size/count limits.

## Acceptance Checks

- [x] API/compile tests cover `UploadBody`, `UploadChunk`, `UploadError`, `upload_error_code`, `upload_error_response`, `UploadSaveOptions`, invalid sync-handler usage, and body-mode validation.
- [x] App/router tests cover route metadata, route table output, OpenAPI `consumes` defaults, body limits, GET body validation, and mutually exclusive extractors.
- [x] HTTP/1 tests cover content-length, chunked upload, `Expect: 100-continue`, early return, `discard()`, oversize, disconnect, and keep-alive eligibility.
- [x] HTTP/2 tests cover DATA streaming, content-length mismatch, stream reset, body-limit rejection, and flow-control backpressure.
- [x] `save_to()` tests cover successful write, overwrite refusal, max-bytes failure, cancellation cleanup, exact-path behavior, no fsync guarantee, and no ring-thread blocking path.
- [x] Upload metrics tests cover started streams, bytes received/consumed, backpressure events, handler cancellation, disconnect, body-too-large, and content-length mismatch.
- [ ] Multipart tests land with `MultipartUpload`: split boundaries, text fields, file data, event shape, metadata lifetimes, malformed boundaries, part-header limits, and parser error reporting.
- [ ] HTTP/3 parity tests are required before marking H3 upload streaming complete.
- [x] Documentation explains lifetime rules, terminal read errors, concurrent-read rejection, early-return semantics, error response mapping, `save_to()` durability/path rules, and the buffered/streaming extractor split.

## Assumptions

- Public API names are frozen as `UploadBody` and `MultipartUpload`.
- Streaming extractors are move-only, async-only, and single-consumer.
- `RequestView::body`, `form`, and `files` remain empty for streaming routes.
- Existing buffered upload APIs remain unchanged.
- Raw `UploadBody` may land before `MultipartUpload` and before full H3 parity if those scopes would otherwise block the core API.
