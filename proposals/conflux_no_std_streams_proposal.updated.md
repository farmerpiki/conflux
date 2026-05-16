# Remove standard stream dependencies — updated review pass

Date: 2026-05-11  
Status: **implemented for reusable-source stream-vocabulary and stderr-print removal**

## Decision delta

The stream cleanup proposal has landed for standard stream vocabulary in reusable sources: `src/` no longer contains `<iostream>`, `<fstream>`, `<sstream>`, standard stream objects/types, `std::istreambuf_iterator`, or `export using std::println` / `std::cerr`. The `build/no-std-streams` CTest gate now enforces that state through `scripts/check_no_std_streams.py`.

Cold stderr diagnostics in reusable sources should not call `std::print/std::println(stderr, ...)` directly. Use `eprint/eprintln` when the source already imports `conflux.utils`; otherwise keep the diagnostic local with a tiny `write(2)` sink so low-level modules do not gain a `conflux_utils` dependency only for logging.

## Current confirmed source state

Reusable library sources no longer using banned stream vocabulary:

```text
src/types.cxx
  no <iostream>, std::println export, std::cerr export, or eprintln implementation

src/net/config.cxx
  config/secret file reads use POSIX helpers plus LineRange parsing

src/net/dns/dns_impl.cxx
  resolv.conf and hosts parsing use read_text_file_nothrow + explicit line parsing

src/net/http_server_impl.cxx
  /proc/self/fdinfo diagnostics use read_text_file_nothrow + LineRange parsing

src/template.cxx
  template load/reload uses blocking_read_text_file_nothrow

src/db/connection.cxx
  SQL file loading uses blocking_read_text_file
```

Reusable library sources also no longer call `std::print/std::println(stderr, ...)` directly. Diagnostics now use:

```text
src/utils.cxx
  eprintln(format(...)) for parse_cidr_list warnings

src/work/carrier_coro.cxx
  local write(2)-based sink for the cold coroutine-frame-pool fallback-rate warning
```

Tests/examples/benchmarks may continue using `std::println` for human output.

## Required Phase 0: make sync file errors independent from uring

Status: `IoError` is now exported from `conflux.types`, `file_io_sync` no longer imports or links `conflux.uring.completion` / `conflux_uring`, and `conflux_file_io_sync` is created/exported as a true no-liburing package component outside the runtime-gated CMake block.

Historical options considered:

```text
Option A, preferred:
  conflux.core exports SystemIoError / IoError
  conflux.uring.completion aliases or imports that type
  conflux.file_io_sync imports core only

Option B:
  conflux.file_io_sync exports FileError
  conflux.file_io maps FileError to IoError at async boundaries
```

With that dependency removed, `file_io_sync` now exposes the needed POSIX-only pieces, including `UniqueFd`, contained open/stat/temp-file helpers, `read_text_file_sync`, `read_file_at_sync`, `read_text_file_nothrow`, and `blocking_*` aliases for direct caller-thread file I/O.

## `UniqueFd` placement

Implemented as proposed: `UniqueFd` lives in `conflux.file_io_sync`, not in the async `FileReader`/`FileHandle` layer. Async `FileReader` still uses uring-aware handles internally.

## Implemented helper set

`conflux.file_io_sync` now provides:

```cpp
export struct FileStat { ... };

export expected<UniqueFd, FileIoSyncError> openat_contained_sync(
    int root_fd,
    string_view relative,
    int flags,
    mode_t mode = 0
) noexcept;

export expected<FileStat, FileIoSyncError> stat_at_sync(
    int dir_fd,
    string_view path,
    int flags = 0,
    unsigned mask = STATX_BASIC_STATS
) noexcept;

export expected<FileStat, FileIoSyncError> fstat_sync(int fd) noexcept;

export expected<void, FileIoSyncError> write_all_fd(int fd, span<byte const> bytes) noexcept;

export expected<string, FileIoSyncError> read_text_file_sync(
    string_view path,
    size_t max_bytes = 16 * 1024 * 1024
);

export optional<string> read_text_file_nothrow(
    string_view path,
    size_t max_bytes = 16 * 1024 * 1024
) noexcept;
```

`write_all_fd` remains blocking-fd-only. On `EAGAIN`/`EWOULDBLOCK`, return an error; do not spin.

## Implemented LineRange placement

Line-view helpers live in `conflux.utils` and remain independent from file I/O:

```cpp
export struct LineView {
    string_view text;
    size_t line_no;
};

export class LineRange { ... };
export string_view strip_cr(string_view) noexcept;
export string_view trim_ascii(string_view) noexcept;
export optional<pair<string_view, string_view>> split_once(string_view, char) noexcept;
```

Consumers own/read the buffer through `file_io_sync`, then parse views through `LineRange`.

## Implemented eprint/eprintln placement

Diagnostics moved from `types.cxx` to `utils.cxx`:

```cpp
export void eprint(string_view message) noexcept;
export void eprintln(string_view message) noexcept;
```

Implementation constraints:

```text
no iostream
no FILE*
no std::format in eprint/eprintln themselves
write to STDERR_FILENO using write loop
never throw
append newline via stack buffer or second write
```

This removes `<iostream>`, `std::println`, and `std::cerr` from `conflux.types`.

## Migration status

```text
[x] Move/add core error type; add file_io_sync UniqueFd and POSIX helpers.
[x] Add LineRange and trim/split helpers to utils.
[x] Move eprint/eprintln to utils and remove stream exports from types.
[x] Replace config_from_ini ifstream path.
[x] Replace DNS tolerant parsers with read_text_file_nothrow.
[x] Replace template load/reload whole-file reads.
[x] Replace DB SQL file loader.
[x] Replace HTTP fdinfo diagnostics.
[x] Add scripts/check_no_std_streams.py gate after sources are clean.
[x] Disallow direct `std::print/std::println(stderr, ...)` diagnostics in reusable sources.
```

## Enforcement update

The original enforcement rule is good, but add this exception:

```text
Allowed in src/ only when inside scripts/generated compatibility test file:
  no standard stream usage unless the test specifically verifies stream interop.
```

Everything else under reusable `src/` fails for:

```text
#include <iostream>, <fstream>, <sstream>, <iosfwd>, <syncstream>
std::ifstream/ofstream/fstream
std::istream/ostream
std::stringstream/istringstream/ostringstream
std::istreambuf_iterator
std::cin/cout/cerr/clog
export using std::println
export using std::cerr
std::print/std::println(stderr, ...)
```

Do not fail `std::format` in this pass. Formatted diagnostics may build strings first, then emit through `eprint/eprintln` or an equivalent local `write(2)` sink.


## Follow-up clarification: blocking/sync/async names

The execution model clarification changes the naming target for future public
APIs: raw syscall-style helpers that may block the calling thread should move
toward `blocking_*` public names; executor-owned non-coroutine chains should use
`sync_*`; coroutine/task APIs should use `async_*`. Existing `*_sync` and
`*_async` names are transitional until the broader pre-v1 renaming pass.
