# HTTP Security Corpus

This corpus tracks parser/security cases that must remain covered by tests or fuzz
inputs before v1. It is intentionally request-smuggling focused because the HTTP
server is web-facing and the parser accepts hostile input.

## Covered in `tests/http_e2e.cxx`

### Request line and headers

- request target longer than the configured line limit returns `414`;
- malformed method token returns `400`;
- empty request target returns `400`;
- oversized single header line returns `431`;
- excessive header count returns `431`;
- obsolete folded header lines return `400`;
- NUL bytes in headers return `400`;
- header lines without `:` return `400`;
- field names containing spaces before `:` return `400`;
- HTTP/1.1 requests with missing or duplicate `Host` return `400`.

### Framing and smuggling

- malformed `Content-Length` returns `400`;
- duplicate `Content-Length` returns `400`, even when values match;
- any request containing both `Content-Length` and `Transfer-Encoding` returns
  `400`;
- a `Content-Length` + `Transfer-Encoding` smuggling attempt followed by a
  pipelined request is closed before the pipelined request can execute;
- unsupported transfer codings return `400`;
- transfer codings after `chunked` return `400`;
- duplicate `Transfer-Encoding` headers return `400`;
- empty transfer-coding tokens return `400`;
- `Transfer-Encoding: CHUNKED` is accepted as the canonical case-insensitive
  form.

### Chunked body bounds

- too many chunks return `400`;
- oversized trailers return `400`;
- huge declared chunks return `413`.

### Timeout hardening

- `request_timeout_ms` and `tls_sniff_timeout_ms` are configured defaults and
  are exercised by the server timeout/reaping path. Add a dedicated slowloris
  E2E test if this path regresses or becomes configurable per listener.

## Maintenance rule

When changing HTTP parser behavior, add the smallest raw-wire request here first,
then add or update the corresponding test/fuzz input. Prefer raw socket E2E
coverage for request-smuggling cases because pipeline and close behavior are part
of the contract, not just parser return codes.
