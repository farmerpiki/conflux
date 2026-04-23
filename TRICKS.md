# Implementation Tricks

Non-obvious decisions that look like bugs but aren't — or that are easy to
accidentally undo. Read this before touching the relevant code.

---

## 1. TLS first-byte sniff sentinel (`http_server.cxx`)

**What:** Both TLS and plain HTTP can arrive on the same port. The server
defers creating the `SSL` object until it sees the first byte of each
connection.

**The sentinel encoding** of `Conn`:

| `ssl` | `tls_hs_done` | Meaning |
|-------|--------------|---------|
| null  | false        | Plain HTTP (either TLS not compiled in, or sniff decided plain) |
| null  | **true**     | **SENTINEL: TLS-capable server, waiting for first byte** |
| non-null | false    | TLS handshake in progress |
| non-null | true     | TLS handshake complete, connection carries application data |

**Flow:**
1. `handle_accept`: if `ssl_ctx != nullptr`, set `tls_hs_done = true` (sentinel).
2. `phase1_copy_recv_bufs`: on first byte, check `ssl == nullptr && tls_hs_done`.
   - Byte `0x16` → TLS ClientHello → `SSL_new()`, `tls_hs_done = false`.
   - Any other byte → plain HTTP → `tls_hs_done = false`.
3. `phase1b_tls_one`: if `!tls_hs_done`, drive `SSL_do_handshake`; on success,
   set `tls_hs_done = true`.

**DO NOT** zero out `tls_hs_done` on accept when `ssl_ctx != nullptr`. That
destroys the sentinel and every connection becomes plain HTTP.

---

## 2. TLS over io_uring: memory BIO pair (`http_server.cxx`)

**What:** io_uring is async — we cannot use OpenSSL's default socket BIOs
(which do synchronous syscalls). Instead, we use two in-memory BIOs.

**Pattern:**
```
  io_uring recv → ciphertext bytes → BIO_write(SSL_get_rbio) → SSL_read → plaintext
  SSL_write / SSL_do_handshake → BIO_read(SSL_get_wbio) → conn.tls_send_buf → io_uring send
```

After every `SSL_do_handshake` or `SSL_read`/`SSL_write` call, drain the wbio
with `tls_flush_wbio()` and queue a send if `tls_send_buf` is non-empty.

**DO NOT** call `SSL_set_fd`. **DO NOT** use `BIO_new_socket`. The ring owns
the I/O — OpenSSL must never touch the socket directly.

---

## 3. GCC module + system header pre-loader (`jwt.cxx`, `http2.cxx`, `http_server.cxx`)

**What:** GCC's `import std;` declares `std::__is_constant_evaluated` with
C++ linkage. Some system headers (`stdlib.h`, transitively pulled by
`<openssl/hmac.h>`, `<nghttp2/nghttp2.h>`, `<openssl/evp.h>`) re-declare it
with C linkage, causing:

```
error: conflicting language linkage for 'std::__is_constant_evaluated'
```

**Fix:** In the global module fragment, include `<openssl/ssl.h>` **first**,
before any other OpenSSL or nghttp2 headers. This pre-populates the header
guard state so the conflicting `stdlib.h` redeclaration is skipped.

```cpp
module;
#include <openssl/ssl.h>   // ← MUST be first; resolves GCC module/header conflict
#include <openssl/evp.h>
#include <nghttp2/nghttp2.h>
```

**DO NOT** reorder these includes or remove the pre-loader. The conflict
silently disappears without it on some GCC versions and returns on others.

---

## 4. GCC TU-local entity rule in module interfaces

**What:** GCC rejects module interface units (`export module foo;`) that
expose TU-local types — types with internal linkage (from anonymous
namespaces) — through standard-library templates. The error looks like:

```
error: 'SomeAnonType' is TU-local but used in 'std::vector<SomeAnonType>'
       which is not TU-local
```

**Affected patterns:**
- `std::vector<AnonType>` as a function parameter or return type in exported
  functions, even if not `export` themselves.
- Lambda types captured into `std::function` or `std::tuple` (leaks through
  template instantiations).

**Fix options (in preference order):**
1. Rewrite to avoid the template instantiation entirely (e.g., `pick_encoding`
   in `compress.cxx` uses per-codec `float` variables instead of
   `vector<EncEntry>`).
2. Move the type out of the anonymous namespace (give it a name) — but then
   it must not collide with other TUs.
3. Move the code to a plain TU (non-module). See test files.

**Test files** are intentionally plain TUs (not module units) for exactly
this reason: `std::thread` with a lambda triggers the rule via
`std::tuple` instantiations inside the thread library.

---

## 5. `uint64_t` in global namespace with `import std;` (GCC)

**What:** GCC's `import std;` places fixed-width integer types (`uint64_t`,
`int64_t`, etc.) only in `std::` — they are not injected into the global
namespace. C APIs (liburing, OpenSSL, etc.) use them from `<stdint.h>` which
expects the global namespace.

**Fix:** Add to the global module fragment:
```cpp
module;
#include <stdint.h>   // uint64_t, uint32_t etc. in global namespace
```

This is only needed in modules that include C library headers that use these
types without the `std::` prefix.

---

## 6. `import std.compat;` alongside `import std;`

**What:** `import std;` gives you the C++ standard library with names in
`std::`. `import std.compat;` additionally injects the C standard library
names into the global namespace (equivalent to including the C headers).

Use `import std.compat;` whenever the module body uses global-namespace C
names directly (`memcmp`, `size_t`, `FILE*`, etc.) that come from C headers
transitively included in the global module fragment — to avoid redeclaration
conflicts.

---

## 7. Port-0 race-free signaling (`http_server.cxx`)

**What:** Tests pass `port=0` so the OS assigns a free port. The port is
known after `bind()` but before the io_uring event loop starts. We need to
signal it to the test thread without a race.

**Pattern:**
1. `getsockname()` after `bind()` → store in `bound_port`.
2. Signal via `std::atomic<uint16_t>` with `store(..., memory_order_release)`
   + `notify_all()` **before** io_uring setup completes.
3. Test calls `srv->port()` which internally waits with `atomic::wait(0, ...)`.

**DO NOT** move the port signal after `io_uring_queue_init_params`. The ring
runs on the same thread — the signal must happen before `run_loop()` blocks.

---

## 8. fd-table generation counter (`http_server.cxx`)

**What:** io_uring CQEs arrive asynchronously. A file descriptor slot can be
reused for a new connection before all pending CQEs from the old connection
are drained. Stale CQEs would corrupt the new connection's state.

**Pattern:**
- Each `Conn` has a `gen` (generation) counter.
- Every SQE packs `gen` into its `user_data` via `pack(op, gen, fd)`.
- Every CQE handler checks `fd_table[fd].gen != cqe_gen` → ignore stale CQE.
- `conn_erase()` does `++conn.gen` to invalidate in-flight CQEs from the old
  tenant **before** resetting other fields.

**DO NOT** reset `gen` to 0 in `conn_erase`. The increment is the invalidation.

---

## 9. Multishot accept + provided buffer ring (`http_server.cxx`)

**What:** A single `io_uring_prep_multishot_accept` SQE generates an
unlimited stream of accept CQEs (one per incoming connection) until cancelled.
`IORING_CQE_F_MORE` in the CQE flags indicates more CQEs will follow; only
re-arm when it's absent.

For recv, `IOSQE_BUFFER_SELECT` + a registered buffer ring lets the kernel
pick a free buffer at CQE time (zero-copy from kernel perspective). The chosen
buffer ID is returned in `cqe->flags >> IORING_CQE_BUFFER_SHIFT`. After
reading, call `return_buffer(bid)` to recycle it:

```cpp
io_uring_buf_ring_add(...);
io_uring_buf_ring_advance(buf_ring, 1);
```

Buffer count is `entries * 4` (4 buffers per ring entry) to absorb bursts.

**DO NOT** call `return_buffer` before reading the data — `io_uring_buf_ring_add`
makes the buffer immediately available to the kernel.

---

## 10. `get_sqe()` retry-on-full (`http_server.cxx`)

**What:** The submission queue can be full if many operations are outstanding.
`io_uring_get_sqe` returns null when full.

**Pattern:** Flush pending SQEs with `io_uring_submit`, then retry once. If
still null, log and return null (callers must handle null gracefully by
skipping the enqueue).

Do **not** spin or loop — the single retry is enough because we flush before
checking again.

---

## 11. `std::chrono::file_clock::to_sys()` + `time_point_cast` (`router.cxx` / `static_files`)

**What:** Clang 21 does not implement `std::chrono::clock_cast`. Converting
`std::filesystem::file_time_type` (which uses `std::chrono::file_clock`) to
`system_clock::time_point` requires:

```cpp
// NOT this (missing in Clang 21):
auto t = std::chrono::clock_cast<std::chrono::system_clock>(lwt);

// DO this instead:
auto t = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
    std::chrono::file_clock::to_sys(lwt));
```

`file_clock::to_sys()` is available in Clang 21 and handles the epoch
difference between `file_clock` and `system_clock`.

---

## 12. Router: middleware params are preserved (not overwritten) (`router.cxx`)

**What:** When a route matches, the dispatch loop extracts URL-segment
captures into a local `HttpFields params`. It then creates `matched = r`
(copying the middleware-modified request) and **appends** URL captures rather
than overwriting:

```cpp
HttpRequest matched = r;
for (auto &[k, v] : params) { matched.params.emplace_back(move(k), move(v)); }
```

**Why:** Middleware (e.g., `jwt_middleware`) injects claims into
`req.params` before calling `next(req)`. If the dispatch loop overwrote params,
those injected values would be lost. Route captures and middleware params now
coexist — later entries win for duplicate keys (consistent with middleware
execution order).

**DO NOT** change back to `matched.params = std::move(params)`.

---

## 13. OpenSSL 3 EVP_MAC for HMAC-SHA256 (`jwt.cxx`)

**What:** The old `HMAC()` API (from `<openssl/hmac.h>`) is deprecated in
OpenSSL 3 and, more importantly, including `<openssl/hmac.h>` in a GCC module
unit triggers the language-linkage conflict (see Trick #3).

**Fix:** Use the `EVP_MAC` API (`<openssl/evp.h>` + `<openssl/params.h>`):

```cpp
EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
EVP_MAC_free(mac);
// set params: digest = SHA256
EVP_MAC_init(ctx, key, key_len, params);
EVP_MAC_update(ctx, msg, msg_len);
EVP_MAC_final(ctx, nullptr, &out_len, 0); // query size
EVP_MAC_final(ctx, out, &out_len, out_len);
EVP_MAC_CTX_free(ctx);
```

**DO NOT** switch back to `HMAC()` — it breaks the GCC module build.

---

## 14. `http_redirect_to_https` same-port redirect (`http_server.cxx`)

**What:** When `Config::http_redirect_to_https = true`, the server listens on
a single port for both HTTP and HTTPS. A TLS ClientHello (byte `0x16`) goes
through the TLS path; anything else is plain HTTP, which gets a 301 redirect
to the HTTPS equivalent URL (same host, same path).

The redirect is issued in `dispatch_request` **before** route matching, using
`req.is_tls` (set by the sniff path). The `host` header from the plain
request is preserved in the redirect URL.

**Configuration:** set `Config::http_redirect_to_https = true` and provide
a `tls_cert`/`tls_key`. No second port or second server instance needed.

---

## 15. Compress: per-codec float q-values (avoid TU-local vector) (`compress.cxx`)

**What:** A natural implementation of Accept-Encoding parsing would collect
`{name, q}` pairs into a `vector<EncEntry>` where `EncEntry` is an
anonymous-namespace struct. This triggers Trick #4 (TU-local entity exposure).

**Fix:** Track q-values as four plain `float` variables:
`q_br`, `q_zstd`, `q_gzip`, `q_star`. Initialised to -1 (not seen). Update
per token, apply wildcard at the end, pick best with `>` comparisons.

Tie-breaking by codec quality (br > zstd > gzip) falls out naturally from the
order of the `if (q_X > best)` checks (strict `>` doesn't update on ties).

**DO NOT** refactor to use a struct/vector — it will fail to compile as a
module interface.

---

## 16. HTTP/2: lazy session setup, batched send (`http_server.cxx`)

**What:** nghttp2 session setup is deferred until the first application-data
recv after the TLS handshake completes with ALPN "h2". This avoids conflicts
with in-flight TLS handshake sends.

**Send batching:** `h2_setup_conn` submits the server SETTINGS frame but does
NOT flush it. The caller (`phase2_build_responses`) then runs
`nghttp2_session_mem_recv` (which queues SETTINGS_ACK), then calls `h2_do_send`
once. A single `nghttp2_session_send` drains all queued frames in one call,
and a single `h2_flush_pending` → `SSL_write` + `io_uring send` delivers them
in one TLS record.

**In-flight accumulation:** If a send is already in flight (`send_queued=true`),
`h2_flush_pending` returns early. Subsequent `h2_do_send` calls (from later
recvs) accumulate output in `h2_pending_send`. When the current send completes,
`handle_send_tls_complete`'s H2 branch calls `h2_do_send` to drain accumulated
data and queue the next send.

**DO NOT** call `h2_do_send` inside `h2_setup_conn` — it would split the
server SETTINGS and the SETTINGS_ACK into two separate io_uring sends, and the
SETTINGS_ACK would be silently dropped if a send is already in flight.

---

## 17. HTTP/2 stream state lives in `std::map<int32_t, H2Stream>` (`http_server.cxx`)

**What:** H2Stream stores both request data (headers, body) and response data
(response_body, response_off for the nghttp2 data provider callback). The
`prd.source.ptr = &stream` pointer passed to `nghttp2_submit_response` must
remain valid until nghttp2 calls `h2_read_cb` for the last time (when
`NGHTTP2_DATA_FLAG_EOF` is returned).

**Why `std::map` not `vector<pair<>>`:** `std::map` node-based storage gives
stable references under insert/erase. Needed because `h2_on_stream_close_cb`
erases completed streams from within `nghttp2_session_send` while the data
provider pointer to another stream is still live. A `std::vector` would
invalidate all pointers on erase.

**DO NOT** change to vector or unordered_map — pointer stability is required.

---

## 18. ALPN selection: use `SSL_select_next_proto`, not `nghttp2_select_next_protocol` (`http2.cxx`)

**What:** `nghttp2_select_next_protocol` only recognises h2 and h2-14. When
the client doesn't offer h2, it returns -1 and leaves `*out` undefined. The
original fallback `*out = in; *outlen = in[0]` pointed to the NPN length byte
rather than the protocol name, corrupting the ALPN extension and breaking TLS
handshakes for http/1.1-only clients (e.g., curl without `--http2`).

**Fix:** Use `SSL_select_next_proto` with a server protocol list of
`"\x02h2\x08http/1.1"`. It correctly handles the wire-format length prefixes,
selects h2 when offered, falls back to http/1.1 otherwise, and returns
`OPENSSL_NPN_NEGOTIATED` (1) on success.

**DO NOT** reintroduce `nghttp2_select_next_protocol` as the sole selector.

---

## 19. wss:// handoff: memory BIO → socket BIO, HTTP bytes stripped (`http_server.cxx`)

**What:** After the TLS 101 Switching Protocols response is sent, the connection
is handed off from the io_uring path (memory BIOs) to a blocking `WsConn`
thread (socket BIO).

**Steps in `handle_send_tls_complete`:**
1. Strip the HTTP request bytes from `conn.partial` **before** moving it to
   `initial_buf` — `WsConn` expects only post-header data (pipelined WS bytes,
   if any). Failing to strip causes `WsConn::recv()` to try parsing the HTTP
   headers as WS frames (garbage).
2. Call `SSL_set_fd(orig_ssl, orig_fd)` — this replaces the memory BIOs with a
   socket BIO. The TLS session state is preserved.
3. Clear `O_NONBLOCK` on the fd so the blocking `SSL_read` in `WsConn::fill()`
   works correctly.
4. Increment `conn.gen` and set `conn.fd = -1` before spawning the thread —
   this invalidates any in-flight io_uring CQEs for the old tenant.

**DO NOT** pass `conn.partial` before stripping `request_bytes`. The resulting
garbage WS frames are silently swallowed in a loop, causing `SSL_read` to block
waiting for data that never arrives.

**DO NOT** call `SSL_shutdown()` in `WsConn::close()` for TLS connections. In
blocking mode it waits for the peer's TLS close_notify. WS clients send WS
close frames (application-layer) but typically do not send TLS close_notify
before the TCP close. The correct sequence is `SSL_free()` then
`::shutdown(fd, SHUT_WR)`.

---

## 20. H2 SSE: `NGHTTP2_ERR_DEFERRED` + `nghttp2_session_resume_data` (`http_server.cxx`)

**What:** HTTP/2 SSE streams use nghttp2's data provider callback (`h2_read_cb`)
to push events. When no SSE data is available yet, return
`NGHTTP2_ERR_DEFERRED` to tell nghttp2 to pause the stream. When new data
arrives (SsePoll fires), call `nghttp2_session_resume_data(session, stream_id)`
followed by `h2_do_send()` to wake the stream.

**Fields that track H2 SSE state per `Conn`:**
- `h2_sse_stream_id` (int32_t, -1 = none)
- `h2_sse_pending_wait` (bool) — set after initial headers send, cleared when
  `queue_sse_wait` is called

**Flow:**
1. `h2_on_frame_recv_cb`: detects SSE response (empty body + SSE content-type
   → sse_channel set); stores `sse_channel` in `H2Stream` and `Conn`; sets
   `h2_sse_stream_id` and `h2_sse_pending_wait = true`.
2. Phase2 H2 branch: after `h2_do_send`, if `h2_sse_pending_wait` → call
   `queue_sse_wait(fd)`.
3. `h2_read_cb` for the SSE stream: drain `H2Stream::h2_sse_buf`; if empty and
   channel open → return `NGHTTP2_ERR_DEFERRED`; if channel closed → set
   `NGHTTP2_DATA_FLAG_EOF`.
4. `handle_sse_poll` H2 branch: `nghttp2_session_resume_data` →
   `h2_do_send` → re-arm if channel still open.

**DO NOT** call `h2_do_send` inside `h2_setup_conn` — it would split SETTINGS
and SETTINGS_ACK into two io_uring sends (see Trick #16).

---

## 21. H2 response trailers: `NGHTTP2_DATA_FLAG_NO_END_STREAM` + `nghttp2_submit_trailer` (`http_server.cxx`)

**What:** To send HTTP/2 trailers (headers after the DATA frames), the data
provider callback must signal that more headers follow before signalling EOF:

```cpp
// In h2_read_cb, when all body data is consumed and trailers exist:
*data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
*data_flags |= NGHTTP2_DATA_FLAG_EOF;
// Build nv[] from response_trailers and call:
nghttp2_submit_trailer(session, stream_id, nv, nv_count);
stream.response_trailers.clear();
```

`NGHTTP2_DATA_FLAG_NO_END_STREAM` tells nghttp2 that the DATA frame is not the
last frame on the stream (trailers follow). Without it, nghttp2 closes the
stream immediately after the last DATA frame, and `nghttp2_submit_trailer`
returns `NGHTTP2_ERR_INVALID_STATE`.

Setting both flags together is intentional and correct — it means "no more data
(EOF), but don't end the stream yet (trailers pending)."

**DO NOT** set only `NGHTTP2_DATA_FLAG_EOF` when trailers are present — the
stream will be closed before the trailers can be sent.

---

## 22. `conflux.file_io` direct-slot gen counter (`file_io/file_io.cxx`)

**What:** `CompletionTable` stores completion callbacks in a slot-indexed
vector and assigns each submission `(slot, gen)` where `gen` increments every
time the slot is reused. The io_uring `user_data` carries both; `dispatch()`
rejects CQEs whose `gen` doesn't match the current slot `gen`.

**Why:** `io_uring_prep_cancel` does not synchronously reap an in-flight op —
the original CQE still arrives, with `res = -ECANCELED`. If the slot has been
freed and reassigned, the stale CQE would fire the wrong completion.

**Pattern:**
```cpp
auto [slot, gen] = completions.reserve(cb);
io_uring_sqe_set_data64(sqe, encode_ud(slot, gen));
// ... later, on CQE:
completions.dispatch(slot, gen, res, flags);  // stale gen → silent drop
```

This mirrors the connection-fd gen counter (Trick #8). Both must increment on
every reuse, including the cancel-all path, or a cancelled operation can run
into a reassigned slot.

**DO NOT** strip the gen bits from user_data to save space; the slot alone
is ambiguous across cancellation.

---

## 23. `O_DIRECT` pipes + per-ring `PipePool` (`file_io/file_io.cxx`)

**What:** `splice_to_fd` runs two linked SPLICE SQEs (file → pipe → dst). The
intermediate pipe is borrowed from `PipePool`, opened with
`pipe2(O_DIRECT | O_CLOEXEC)` so each write stays as a distinct packet
(cleaner pairing with `SPLICE_F_MOVE`).

**Per-ring, never shared:** io_uring validates fd arguments against the
submitting thread's fd table. A pipe created on thread A and spliced to by
thread B's ring works, *but* the kernel fast-path assumes the fd is visible
to the ring owner; cross-thread pipe sharing has been observed to degrade to
the copy path on some kernels.

**Fallback:** `pipe2(O_DIRECT)` returns `EINVAL` on kernels without
pipe-packet support. `PipePool` retries with plain `pipe2(O_CLOEXEC)` — still
zero-copy on the splice path, just byte-stream semantics.

**DO NOT** share a `PipePool` across rings. Construct one per ring, alongside
`FixedBufferPool`.

---

## 24. `IOSQE_IO_LINK` splice chain: partial splice cancels the tail (`file_io/file_io.cxx`)

**What:** `splice_to_fd` submits two linked SQEs per chunk:

```
SQE A: splice(file → pipe, chunk, MOVE|MORE)  with IOSQE_IO_LINK
SQE B: splice(pipe → dst,  chunk, MOVE|MORE)
```

When SQE A returns short (`res < chunk`, e.g. end-of-file or pipe-full),
the kernel cancels the linked tail — SQE B's CQE arrives with
`res = -ECANCELED`. This is not an error; the library's out-side callback
detects `ECANCELED` when the source already reported a partial result and
re-submits the remaining bytes.

**Why:** `SPLICE_F_MORE` hints that more is coming but does not wait; `MOVE`
tells the kernel it may move pages rather than copy. The short path is normal
on tight pipes.

**DO NOT** treat `-ECANCELED` on the out-side as an error. The in-side CQE
is the authoritative byte count; the out-side is a hint for chain progress.
