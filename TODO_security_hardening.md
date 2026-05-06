# HTTP Server Security Hardening TODO

## Phase 1 — Security Blockers

- [x] 1. Response header validation
  - Added valid_header_name()/valid_header_value() in http_server.cxx
  - Validates in format_response() — skips invalid entries
  - Blocks framing headers: content-length, transfer-encoding, connection, upgrade, keep-alive, te, trailer
  - Files: src/net/http_server.cxx (format_response ~line 97)

- [x] 2. Static file openat2 containment
  - Replaced stat()+open() with openat2(root_fd, relative, RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS)
  - Applied to GET, PUT, DELETE paths
  - Files: src/net/router.cxx (serve_static, contained_open helper, RootDirFd)

- [x] 3. Async handler safety — reject Task<HttpResponse> from HttpRequestView handlers
  - static_assert in make_handler: HttpRequestView + Task<HttpResponse> = compile error
  - Only HttpRequest const& may return Task<HttpResponse>
  - Files: src/net/router.cxx (make_handler), examples/hello.cxx (fixed usage)

- [x] 4. Request parser CTL rejection + duplicate Host
  - Rejects field_value bytes < 0x20 (except \t) and 0x7F
  - Rejects duplicate Host header (> 1 Host in HTTP/1.1)
  - Files: src/net/http1_parser.cxx, src/net/http_server.cxx

## Phase 2 — Protocol Correctness

- [x] 5. Oversize body duplicate send guard
  - Added `if (conn.send_queued) continue;` before size check
  - File: src/net/http_server.cxx

- [x] 6. 416 Content-Range header
  - Moved "bytes */size" from body to resp.headers["Content-Range"]
  - File: src/net/router.cxx

- [x] 7. Accept-Encoding token parsing
  - Replaced substring find() with proper comma-separated token parsing
  - Respects q=0 (won't select encoding with quality 0)
  - File: src/net/router.cxx

- [x] 8. 204/304 response framing
  - 204: omits Content-Type and Content-Length
  - 304: only emits Content-Length if content_length_hint is set
  - 1xx: omits Content-Length
  - HEAD: emits real Content-Length (matching GET) but suppresses body
  - File: src/net/http_server.cxx (format_response)

- [x] 9. Multishot accept remote_addr
  - Non-fixed-files: uses getpeername() after accept (safe per-CQE)
  - Fixed-files: keeps shared buffer (getpeername unavailable on fixed fd indices)
  - File: src/net/http_server.cxx (handle_accept)

## Phase 3 — Hardening

- [x] 10. TLS SSL_write loop
  - Main response path: loops until all plaintext consumed or error
  - H2 pending_send path: loops SSL_write
  - File: src/net/http_server.cxx

- [x] 11. TLS mapped-file streaming
  - Use streamed-file path for TLS large files instead of full copy into own_response
  - File: src/net/http_server.cxx (~line 1707)
  - Lower priority: works correctly, just memory-inefficient for large TLS file responses

- [x] 12. H2 pending_send cap
  - Add per-connection buffer limit and nghttp2 backpressure on overflow
  - File: src/net/http_server.cxx (h2_pending_send usage)

- [x] 13. Directory listing URL-encode hrefs
  - Added path_percent_encode() for href attribute, html_escape for display text
  - File: src/net/router.cxx

- [x] 14. HTTPS redirect use canonical host from allowlist
  - Uses matched allowlist entry instead of raw Host header value in Location
  - File: src/net/http_server.cxx

## Phase 4 — Review Follow-ups

- [x] 15. Multipart boundary-line parsing
  - Current parser is simple and scans for delimiter text inside the body; production parsing should require real boundary lines, final boundary handling, robust part header parsing, per-part/header limits, and an explicit filename policy.
  - File: src/net/http_server.cxx

- [ ] 16. HTTP/2 request validation parity
  - Validate required pseudo-headers, duplicate pseudo-headers, pseudo-header ordering, lowercase field names, forbidden connection-specific headers, `te` value, and `content-length` consistency.
  - File: src/net/http_server.cxx

- [ ] 17. Chunked wire-overhead limits
  - Decoded body size and chunk count are bounded, but the raw receive cap should intentionally account for small-chunk overhead, chunk extensions, and trailer bytes.
  - File: src/net/http_server.cxx

- [ ] 18. `Expect: 100-continue` stress tests
  - Add cases for headers-only arrival, delayed body, no body, pipelined follow-up request, timeout after provisional response, and chunked bodies with `Expect`.
  - Files: src/net/http_server.cxx, tests/http_e2e.cxx

- [ ] 19. Ring-thread synchronous handler contract
  - Document that synchronous handlers run on the ring thread and are suitable only for short non-blocking work, or provide a default work-pool offload path for slow handlers.
  - Files: src/net/http_server.cxx, docs
