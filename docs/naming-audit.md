# API Naming Audit

This audit records public and near-public names that should be considered during
one later pre-v1 naming pass. It intentionally makes no code changes and should
not be used as permission to rename symbols opportunistically across component
boundaries.

Canonical execution-name families are defined in `docs/execution-model.md`; review
rules and component-boundary semantics are consolidated in
`docs/concurrency-naming-model.md`:

- `blocking_*` — raw blocking syscall-style helpers that may block the caller
  thread directly;
- `sync_*` — executor-owned, non-coroutine user surfaces that run a chain and
  return/report the result synchronously;
- `async_*` — coroutine/task APIs with explicit suspension.

Current `*_sync` and `*_async` names are transitional. The final rename pass
should prefer adding final names first, migrating call sites component-by-
component, and removing aliases only during the release cleanup branch.

## Review rules

1. Do not rename while unrelated implementation branches are active.
2. Do not use `blocking_*` merely because an API is not a coroutine.
3. Do not rename executor-owned sync chains to `blocking_*`; use `sync_*`.
4. Do not rename ordinary pure value/CPU helpers merely to add a prefix.
5. Keep module/target renames separate from function renames unless the component
   is already being split.
6. For public APIs, add aliases and migration notes before removing old names.
7. For internal exported helper modules, prefer narrow local renames with no
   compatibility alias when no downstream public import is intended.

## Blocking syscall-style and file-backed helpers

These names are runtime-free caller-thread blocking helpers. Most already live in
`conflux.file_io_sync`, `conflux.file_map`, or `conflux.json.file`; the issue is
mainly suffix order and a few unprefixed helpers.

| Current name | Current location | Final-shape candidate | Notes |
|---|---|---|---|
| `conflux.file_io_sync` | `src/file_io/file_io_sync.cxx` | keep target for now; later consider `conflux.file_io.blocking` | Module/target rename is broad CMake/docs churn; defer until function aliases exist. |
| `TemporaryFileSync` | `conflux.file_io_sync` | `BlockingTemporaryFile` or `TemporaryFile` in a blocking module | Type suffix mirrors module suffix; rename with file helper batch only. |
| `FileIoSyncError` | `conflux.file_io_sync` | `FileIoError` in a blocking module | `FileIoError` already aliases the same error in async file I/O; avoid ambiguity before target split settles. |
| `open_tmpfile_sync` | `conflux.file_io_sync` | `blocking_open_tmpfile` | Raw `open`/`openat`-style helper. |
| `publish_tmpfile_sync` | `conflux.file_io_sync` | `blocking_publish_tmpfile` | Raw link/rename/fsync-style helper. |
| `write_all_fd` | `conflux.file_io_sync` | `blocking_write_all_fd` | Currently unprefixed despite direct `write` loop. |
| `read_all_fd` | `conflux.file_io_sync` | `blocking_read_all_fd` | Currently unprefixed despite direct `read` loop. |
| `write_file_atomic_at_sync` | `conflux.file_io_sync` | `blocking_write_file_atomic_at` | File-backed convenience around raw blocking helpers. |
| `write_text_file_atomic_at_sync` | `conflux.file_io_sync` | `blocking_write_text_file_atomic_at` | Same batch as binary atomic write. |
| `fstat_sync` | `conflux.file_io_sync` | `blocking_fstat` | Thin statx/fstat-style helper. |
| `stat_at_sync` | `conflux.file_io_sync` | `blocking_stat_at` | Thin statx/openat-style helper. |
| `read_file_at_sync` | `conflux.file_io_sync` | `blocking_read_file_at` | File-backed convenience around raw blocking helpers. |
| `map_fd_readonly_sync` | `conflux.file_map` | `blocking_map_fd_readonly` | May fault later through mmap access; still caller-thread blocking setup. |
| `map_file_readonly_sync` | `conflux.file_map` | `blocking_map_file_readonly` | Uses blocking file open/stat/map helpers. |
| `parse_file_at_sync` | `conflux.json.file` | `blocking_parse_file_at` | Keep separate from pure `json::parse*`; this one performs blocking file I/O. |
| `parse_file_sync` | `conflux.json.file` | `blocking_parse_file` | Same batch as `parse_file_at_sync`. |

Status: `file/blocking-name-aliases` adds these `blocking_*` function aliases
for `conflux.file_io_sync`, `conflux.file_map`, and the file-backed
`conflux.json.file` helpers while keeping the legacy spellings. Internal
call sites and tests now prefer the `blocking_*` names except one compatibility
coverage path for legacy sync spellings. Type/module renames remain deferred to
the release alias-removal pass.

## Coroutine/task APIs with suffix-style async names

These names return `root::Task<T>` or another awaitable. They are semantically
async, but most use suffix style. The final pass should prefer `async_*` names or
an explicit documented exception where the enclosing type/module already makes
coroutine behavior obvious.

| Current family/name | Current location | Final-shape candidate | Notes |
|---|---|---|---|
| `FileReader::*_async` | `src/file_io/file_io.cxx` | `FileReader::async_*` | Preferred aliases landed for the full exported `FileReader` suffix family; old names remain compatibility aliases until release cleanup. |
| `conflux.uring.timeout::{timeout_async, timeout_remove_async, link_timeout_async}` | `src/uring/uring_timeout.cxx` and forwarding methods in `FileReader` | `async_timeout`, `async_timeout_remove`, `async_link_timeout` | Preferred aliases landed; `FileReader::async_timeout_update` covers the update SQE wrapper. |
| `conflux::http::send_async` | `conflux.net.async_client` | `async_send` | `async_send` alias landed; keep `send_async` as compatibility until release alias cleanup. |
| `conflux.net.async_client` | module name | maybe `conflux.net.http.client_async` or keep | Module names do not have to follow function-prefix order; only rename if HTTP client modules are reorganized. |
| `proxy_async` | `conflux.net.proxy` | `async_proxy` | `async_proxy` alias landed; keep `proxy_async` as compatibility until release alias cleanup. |
| `spawn_async_in`, `run_async_in`, `wait_async_in` | `conflux.process` | `async_spawn_in`, `async_run_in`, `async_wait_in` | Preferred names landed; old names remain compatibility aliases until examples/tests finish migrating. |
| `tcp_connect`, `tcp_accept`, `tcp_accept_multishot`, `sleep_for` | `conflux.socket_io.coro` | `async_tcp_connect`, `async_tcp_accept`, `async_tcp_accept_multishot`, `async_sleep_for` | Preferred names landed; old names remain compatibility aliases until release alias cleanup. |
| `TcpStream::{recv_borrowed, recv_owned, write_borrowed, write_copy, write_owned, write_all_* , shutdown, close}` | `conflux.socket_io.coro` | `async_recv_borrowed`, `async_recv_owned`, `async_write_*`, `async_shutdown`, `async_close` | Preferred names landed; old names remain compatibility aliases until release alias cleanup. |
| `UdpSocket::{send_to_borrowed, send_to_copy, recv_from}` | `conflux.socket_io.coro` | `async_send_to_*`, `async_recv_from` | Preferred names landed; old names remain compatibility aliases until release alias cleanup. |

Keep final removal of legacy `FileReader::*_async` names in the release alias-cleanup branch; the preferred aliases are now available and call sites have been migrated.

## Executor-owned synchronous surfaces

These APIs block the caller while waiting for executor-owned work or expose a
non-coroutine shape over executor/ring-owned progress. They should use `sync_*`
when they are public. They should not become `blocking_*` unless they stop using
Conflux task/executor machinery.

| Current name | Current location | Final-shape candidate | Notes |
|---|---|---|---|
| `sync_wait` | `conflux.work` | keep | Already prefix-style and matches familiar async ecosystem terminology. |
| `run_on_task` | `conflux.work` | `async_run_on` | Preferred name landed; old name remains as a compatibility alias. It returns `Task<T>`, so new call sites should use the coroutine/task spelling. |
| `block_on_socket_task` | `conflux.socket_io.blocking` | `sync_wait_socket_task` | Preferred name landed; old name remains as a compatibility alias. |
| `dispatch_sync_routes` | `conflux.net.router_dispatch` | `dispatch_immediate_routes` | Preferred name landed; old helper remains as a compatibility alias. Internal exported helper; not executor-owned sync API, just immediate route dispatch on current ring thread. |
| `Router::dispatch` | `conflux.net.router` | keep | Immediate in-process dispatch; ordinary method name is clear. |

## Names containing `async` but not returning an awaitable

These are the highest semantic-risk names because `async_*` should mean a
coroutine/task API. If they stay non-coroutine, prefer names that describe route
class, context dispatch, or deferred response creation instead of async execution.

| Current name | Current location | Final-shape candidate | Notes |
|---|---|---|---|
| `Router::dispatch_async` | `conflux.net.router` | `dispatch_context` | Preferred name landed; old name remains as a compatibility alias. |
| `VHostRouter::dispatch_async` | `conflux.net.vhost` | `dispatch_context` | Preferred name landed; old name remains as a compatibility alias. |
| `try_dispatch_async` | `src/net/http_server_impl.cxx` | `try_dispatch_context` | Internal helper renamed. |
| `dispatch_async_routes` | `conflux.net.router_dispatch` | `dispatch_context_routes` | Preferred name landed; old name remains as a compatibility alias. |
| `router_run_async_http_task` / `Router::run_async_http_task` | router dispatch internals | `router_defer_http_task` / `Router::defer_http_task` | Preferred names landed; old names remain compatibility aliases. |
| `conflux.net.http.static_async` | static-file module | decide later | The module contains async/static helpers and blocking write fallbacks. Split route registration/static I/O first, then rename. |

Suggested branch shape: one `router/dispatch-naming-aliases` branch after static
route registration split. This should be separate from HTTP event-loop work.

## Blocking HTTP/proxy names needing explicit decision

These names are caller-thread blocking, but they are higher-level than the strict
raw-syscall `blocking_*` definition. Do not rename them blindly.

| Current name | Current location | Options | Notes |
|---|---|---|---|
| `HttpClient::send_blocking` | `conflux.net.client` | `blocking_send` | Preferred name landed; old method remains as a compatibility alias. Current implementation uses blocking socket/poll/TLS code directly, so `sync_send` would be wrong unless implementation changes. |
| `client_detail::do_blocking_request` | `src/net/client.cxx` | internal `blocking_request` or keep internal | Internal helper; rename only if public method changes. |
| `proxy_sync` | `conflux.net.proxy` | `blocking_proxy` | Preferred name landed; old function remains as a compatibility alias. It calls `HttpClient::blocking_send`, so avoid using it on HTTP ring threads unless explicitly offloaded. Prefer `async_proxy` in context routes. |

The old `todo/final syntax before release.md` text allowed broad high-level
`blocking_*` convenience APIs. Current policy is narrower. Treat the execution
model and this audit as canonical until that todo file is either rewritten or
removed.

## Ordinary names that should stay ordinary

These names do not need execution prefixes merely because they perform work:

- pure JSON value APIs such as `parse`, `parse_view`, `parse_copy`, `write`,
  `stringify`, and provider-boundary helpers;
- pure HTTP value builders such as `Response::json`, `ClientRequest::Builder`, URL
  parsing, header manipulation, cookie formatting, ETag/cache-control helpers;
- low-level SQE submission helpers named `submit_*`, because they prepare work on
  a ring and report immediate submission success/failure rather than blocking or
  suspending;
- router registration methods such as `get`, `post`, `use`, `group`, `serve_static`,
  and `sse`.

## Release cleanup order

1. Finish active implementation branches that touch the same components.
2. Add final names plus deprecated aliases in component-local branches.
3. Migrate tests, examples, and docs to final names.
4. Update migration docs with old-to-new tables.
5. Remove aliases only in `release/remove-aliases` after all feature branches are
   merged.
