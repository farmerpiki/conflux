# Cost/lifetime model

This page answers the operational questions that matter at API boundaries:
does this copy, does this allocate, does it borrow, can it block, and how long
is the data valid?

The short rule for HTTP is: the server owns the request storage, while handlers,
middleware, async context routes, and extractor views receive `http::Request`
/ `http::RequestView` views over that storage. Code that needs to retain request
data past the server-owned lifetime must copy the needed fields or explicitly
materialize `http::OwnedRequest`.

## HTTP request lifetimes

| Type | owns memory? | borrows from? | valid until | can suspend? | can cross thread? | can be stored? | copies? | allocates? |
|---|---|---|---|---|---|---|---|---|
| `http::RequestView` | No | Server-owned request storage for the current dispatch | Server-owned dispatch/task lifetime | Yes, while the server-owned request remains live | No unless the server dispatch contract says so | No | No | No |
| `http::Request` | No; alias for `http::RequestView` | Server-owned request storage for the current dispatch | Server-owned dispatch/task lifetime | Yes, while the server-owned request remains live | No unless the server dispatch contract says so | No | No | No |
| `http::OwnedRequest` | Yes | Nothing after construction | Object lifetime | Yes | Yes, subject to normal C++ synchronization | Yes | Yes, from request buffers | Yes |
| `http::Path<...>` | Usually no for view/scalar extraction | `req.params` | Server-owned dispatch/task lifetime for normalized app routes; otherwise caller-owned request lifetime | Yes for normalized app async handlers while the server-owned request remains live | No when borrowed | Only if copied/owned | String targets copy; numeric targets parse | String copies allocate; numeric success does not |
| `http::Query<...>` | Usually no for view/scalar extraction | `req.query` | Server-owned dispatch/task lifetime for normalized app routes; otherwise caller-owned request lifetime | Yes for normalized app async handlers while the server-owned request remains live | No when borrowed | Only if copied/owned | String targets copy; numeric targets parse | String copies allocate; numeric success does not |
| `http::Header<...>` | Usually no for view/scalar extraction | `req.headers` | Server-owned dispatch/task lifetime for normalized app routes; otherwise caller-owned request lifetime | Yes for normalized app async handlers while the server-owned request remains live | No when borrowed | Only if copied/owned | String targets copy; numeric targets parse | String copies allocate; numeric success does not |
| `http::Cookie<...>` | Usually no for view/scalar extraction | `req.cookies` | Server-owned dispatch/task lifetime for normalized app routes; otherwise caller-owned request lifetime | Yes for normalized app async handlers while the server-owned request remains live | No when borrowed | Only if copied/owned | String targets copy; numeric targets parse | String copies allocate; numeric success does not |
| `http::BodyBytes` | No for `std::span<std::byte const>` view form | Request body buffer | Server-owned dispatch/task lifetime for normalized app routes; otherwise caller-owned request lifetime | Yes for normalized app async handlers while the server-owned request remains live | No when borrowed | Only if copied/owned | No for view form | No for view form |
| `http::BodyText` | No for view form | Request body buffer | Server-owned dispatch/task lifetime for normalized app routes; otherwise caller-owned request lifetime | Yes for normalized app async handlers while the server-owned request remains live | No when borrowed | Only if copied/owned | No for view form | No for view form |
| `http::Json<T>` | Depends on `T` and provider options | Request body and/or provider-owned document | Borrowed values follow input/document lifetime; owned values follow object lifetime | Only after owning decode/document materialization | Only when decoded value is thread-safe and no borrowed input escapes | Only if decoded value owns its referenced data | Provider and `T` dependent | Provider and `T` dependent |
| `http::State<T>` | No; reference/shared access to app state | Application-owned state | State object lifetime | Yes if the state access remains valid | Yes if `T` is safe for that use | Store handles only when their ownership semantics allow it | No by default | No by default |
| `UploadedFile` | No for `RequestView`; yes after `to_owned()` | Multipart request body and header storage | End of synchronous handler call, unless converted with `to_owned()` | No when borrowed | No when borrowed | Only after `to_owned()` or manual copy | `to_owned()` copies fields and bytes | `to_owned()` allocates |

View extractors are for inspection while the server-owned request remains live.
If data must survive that lifetime or a handoff to another thread, copy it into
`std::string`, decode it into an owning value, or convert the whole request to
`http::OwnedRequest`.

Borrowed extractors and `http::RequestView` may suspend only through normalized
server/app async dispatch paths that pin the request storage for the deferred
task lifetime and keep the view object in the coroutine chain. Raw caller-owned
`RequestView` task dispatch has the caller's lifetime contract: keep the backing
storage alive until the deferred response completes, or use `http::OwnedRequest`
/ copy the fields the task needs.

## HTTP response costs

| Response | body ownership | copies body? | allocates? | blocks? | zero-copy eligible? | TLS caveat? | validity requirement |
|---|---|---|---|---|---|---|---|
| `http::text(...)` | Response owns `std::string` body | Moves rvalues; copies if caller constructs from borrowed data | Yes for body/status/header storage as needed | No | No; sent from owned memory | TLS still copies through TLS stack | Body is owned by response |
| `http::owned_text(...)` | Response owns caller-provided `std::string` body | Moves the supplied string into the response | No extra body copy; response metadata may allocate | No | No; sent from owned memory | TLS still copies through TLS stack | Caller gives up the string |
| `http::html(...)` | Response owns `std::string` body | Moves rvalues; copies if caller constructs from borrowed data | Yes as needed | No | No; sent from owned memory | TLS still copies through TLS stack | Body is owned by response |
| `http::owned_html(...)` | Response owns caller-provided `std::string` body | Moves the supplied string into the response | No extra body copy; response metadata may allocate | No | No; sent from owned memory | TLS still copies through TLS stack | Caller gives up the string |
| `http::json(...)` | Response owns already-serialized JSON string | Moves rvalues; serialization may already have copied | Yes as needed | No | No; sent from owned memory | TLS still copies through TLS stack | Pass valid serialized JSON bytes |
| `http::ok(...)` | Response owns any supplied body | Same as selected body helper | Yes when body/header storage is created | No | Body dependent | TLS may disable kernel zero-copy paths | Body must be owned or copied before return |
| `http::created(...)` | Response owns any supplied body and headers | Same as selected body helper | Yes when body/header storage is created | No | Body dependent | TLS may disable kernel zero-copy paths | Body and `Location` header must be owned by response |
| `http::owned_created(...)` | Response owns caller-provided `std::string` body | Moves the supplied string into the response | No extra body copy; headers/status may allocate | No | Body dependent | TLS may disable kernel zero-copy paths | Caller gives up the string |
| `http::no_content()` | No body | No | Minimal response metadata only | No | Not relevant | Not relevant | No body data |
| `http::blocking_file_response(...)` (`conflux.http.extended`) | Response owns a `std::string` body read from disk | Yes; reads the whole file into the response body | Yes for the body buffer | Yes; performs blocking filesystem I/O on the caller | No; use static/file streaming paths for zero-copy or async sends | TLS still copies through TLS stack | File contents are copied before return |
| `http::buffered_stream(...)` | Response owns a `std::string` built by `StreamSink` | Yes; chunks append into the response buffer | Yes as the buffer grows | Writer must not block ring thread | No; this is buffered, not incremental streaming | TLS still copies through TLS stack | Chunks only need to survive each `write()` call |
| `http::sse(...)` | `SseChannel` owns queued events | Event sends copy into channel buffers | Yes per queued event | Send call should not block | No | TLS sends through TLS path | Channel must outlive active SSE response |
| `http::deferred(...)` | `DeferredResponse` owns completion state | Completion response dependent | Yes for shared/deferred state | Waiting must not block ring thread | Completion response dependent | TLS may disable kernel zero-copy paths | Complete exactly once while shared state is alive |

File and static responses may avoid body copies, but `stat`, `open`, cache
lookup, range handling, and metadata construction can still do work unless the
entry is already cached. TLS may disable kernel zero-copy paths even when the
plain HTTP path could have used them.

## JSON ownership and allocation

| API | input ownership | string ownership | arena/PMR behavior | copies? | allocates? | error path allocations? | UTF-8 validation? | duplicate key behavior? |
|---|---|---|---|---|---|---|---|---|
| `parse_borrowed` / borrowed document | Caller owns input | String views borrow from input | Provider dependent | No for input/string storage | Parser metadata may allocate | May allocate diagnostic text | Strict validation unless options/provider say otherwise | Controlled by parse options; default should reject |
| `parse_owned` / owning document | Document owns copied or moved input | Strings refer to document-owned storage or provider storage | Provider dependent | Copies lvalue input; moves rvalue input | Yes for owned buffer/metadata | May allocate diagnostic text | Strict validation unless options/provider say otherwise | Controlled by parse options; default should reject |
| `parse_into` arena/PMR | Caller owns input unless policy copies it | Depends on selected arena/PMR policy | Uses caller-supplied arena/resource for eligible storage | Policy dependent | Through supplied arena/resource when supported | May still allocate diagnostics outside arena | Strict validation unless options/provider say otherwise | Controlled by parse options; default should reject |
| `decode<T>` | Follows source document/reader/input | Depends on `T`; `std::string_view` can borrow, `std::string` owns | Provider and codec dependent | Depends on `T` and codec | Depends on `T` and codec | May allocate diagnostics | Decode source should already be validated, or reader validates while decoding | Source parser policy decides object duplicate handling |
| `write_json` / `dump_json` | Caller owns value being serialized | Output sink/string owns emitted bytes | Provider dependent | `dump_json` creates an owning output string; `write_json` may stream to sink | `dump_json` allocates output; writer may allocate for scratch | May allocate diagnostics | Output is generated as valid UTF-8 when inputs are valid | Not applicable unless serializing duplicate-capable object model |
| `Json<T>` request extractor | Request body is input; decoded `T` may own or borrow | Depends on `T` and provider options | Provider dependent | Provider and `T` dependent | Provider and `T` dependent | May allocate rejection/diagnostic text | Validates while parsing/decoding | Provider parse options decide behavior |
| `Json<T>` response wrapper | Caller owns `T` until serialized | Serialized response owns emitted bytes unless streaming provider is used | Provider dependent | Usually copies into response output; streaming provider may reduce intermediate copy | Yes for response output unless direct streaming path is used | May allocate diagnostics | Output must be valid UTF-8 JSON | Not applicable for normal typed objects |

The framework contract is provider-neutral. Native-provider behavior documents
one implementation, not a requirement that every provider use the same DOM,
arena, string interning, or writer strategy.

## Runtime blocking model

| API/path | runs on caller? | blocks caller? | blocks ring thread? | requires explicit opt-in? | safe inside HTTP handler? |
|---|---|---|---|---|---|
| `blocking_*` APIs | Yes | Yes | Yes, if caller is a ring thread | Yes; name advertises it | No, except tiny bounded operations proven harmless |
| `sync_*` APIs | Caller enters a synchronous surface; work is executor-owned when applicable | May block caller waiting for completion | Should not block ring thread unless explicitly documented | Yes by API choice | Usually no from ring handlers if it waits |
| `async_*` APIs | Progress is executor/task-owned after start | Suspends instead of blocking caller | No, unless coroutine body performs blocking work | Yes by coroutine/task use | Yes when awaited work is non-blocking and request views stay within the server-owned lifetime |
| `root::blocking_join` | Yes | Yes | Yes, if called on a ring thread | Yes | No |
| HTTP handler | Ring thread | Handler runs inline until it returns | Yes if handler performs blocking or heavy work | Handler registration is explicit | Yes for bounded parsing, routing, response construction |
| HTTP async handler | Starts on ring/task executor path | Suspends instead of blocking when awaiting async work | No unless coroutine body blocks | Must keep request views within server-owned lifetime or copy explicitly | Yes when awaited work is non-blocking |
| `http::offload` / worker pool path | Enqueues work from caller; work runs on worker | Caller should not block while queued work runs | No for the offloaded work | Yes | Yes for blocking/heavy work that has been explicitly moved |
| WebSocket `WsConn` backend | Upgraded connection is owned by a worker | Yes, for slow peer sends/receives on that worker | No after handoff; ring thread is not the WS I/O owner | Yes through `App::ws` / `WsUpgrade` | Use only when blocking per-connection worker semantics are acceptable |
| `HttpServer::drain` | Caller requests drain; rings perform connection work | No long wait in the API itself; `run()` returns when rings finish | Wakes ring threads; deadline close work runs on rings | Yes | Yes from a controller thread, not from a handler |
| File mmap setup | Caller performs setup/syscalls | May block on filesystem metadata/page setup | Yes, if caller is a ring thread | Yes through file/static setup path | Avoid in hot handler path unless cached or explicitly acceptable |
| Socket send/recv task | Ring/task executor | No ordinary caller blocking | Ring thread progresses I/O events; task code must not block | Yes through async/socket API | Yes for non-blocking socket task operations |

HTTP handlers run on the server's `io_uring` ring threads. The framework does
not silently move arbitrary synchronous handlers to a worker pool; use explicit
offload when blocking disk I/O, DNS, client HTTP, DB calls, sleeps, or heavy CPU
work is required.

## Do not do this

```cpp
// Bad: stores request view after handler returns.
std::string_view leaked;
app.get("/", [&](http::RequestView req) {
    leaked = req.path;
    return http::ok();
});
```

```cpp
// Good: copy/own before storing.
std::string saved;
app.get("/", [&](http::RequestView req) {
    saved = std::string(req.path);
    return http::ok();
});
```

```cpp
// Bad: borrows JSON strings from an input buffer that is about to die.
auto name_from_temporary() -> std::string_view {
    auto doc = json::parse_borrowed(load_body());
    return doc->root().as_object()->member("name")->as_string().value();
}
```

```cpp
// Good: keep the input/document alive, or decode/copy into an owning type.
auto name_from_body(std::string body) -> std::string {
    auto doc = json::parse_owned(std::move(body));
    return std::string(doc->root().as_object()->member("name")->as_string().value());
}
```

```cpp
// Bad: response body points at a temporary after return.
app.get("/bad", [] {
    std::string body = "hello";
    return http::text(std::string_view{body});
});
```

```cpp
// Good: return an owning response body.
app.get("/good", [] {
    return http::text(std::string{"hello"});
});
```

- Do not block inside handlers.
- Do not store borrowed JSON views past input or document lifetime.
- Do not borrow response body data from a temporary.
- Do not assume zero-copy under TLS.
- Do not benchmark debug or sanitizer builds when making performance claims.
