# Remove standard stream dependencies — updated review pass

Date: 2026-05-11  
Status: **recommended; Phase 0 core-error/file_io_sync prerequisite is partially unblocked**

## Decision delta

The stream cleanup proposal is correct, and the original core-error prerequisite is now partially unblocked: `file_io_sync` no longer depends on `conflux.uring.completion`. The remaining ordering constraint is the public component split, because the current CMake target is still created inside the runtime-gated block.

Finish the target split before replacing streams in components that must be usable without liburing.

## Current confirmed stream inventory

Reusable library sources still using stream vocabulary:

```text
src/types.cxx
  #include <iostream>
  export using std::println
  export using std::cerr
  eprintln() implemented via std::println(std::cerr, ...)

src/net/config.cxx
  std::ifstream + std::getline for INI config

src/net/http_server.cxx
  std::ifstream for /proc/self/fdinfo/<ring> diagnostics

src/template.cxx
  std::ifstream + std::istreambuf_iterator for template load/reload

src/work/carrier_coro.cxx
  std::print(stderr, ...) cold warning

src/db/connection.cxx
  ifstream + istreambuf_iterator for SQL file loading
```

This inventory is now one item smaller: `src/net/dns/dns_impl.cxx` no longer uses `std::ifstream` / `std::getline` for `/etc/resolv.conf` or `/etc/hosts`; it uses a bounded POSIX read helper plus an explicit line splitter instead.

This matches the original proposal. Tests/examples/benchmarks may continue using `std::println` for human output.

## Required Phase 0: make sync file errors independent from uring

Status: `IoError` is now exported from `conflux.types`, and `file_io_sync` no longer imports or links `conflux.uring.completion` / `conflux_uring`. The remaining prerequisite for a true no-liburing `file_io_sync` package is the modular target split that moves `conflux_file_io_sync` out from the current runtime-gated CMake block.

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

With that dependency removed, `file_io_sync` can safely expose:

```cpp
expected<string, FileIoError> read_text_file_sync(...);
expected<vector<byte>, FileIoError> read_binary_file_sync(...);
expected<void, FileIoError> write_all_fd(...);
expected<FileStat, FileIoError> stat_at_sync(...);
expected<void, FileIoError> fsync_fd_sync(int fd) noexcept;
expected<void, FileIoError> fsync_dir_sync(int dir_fd) noexcept;
```

## Add `UniqueFd` to file_io_sync, not FileHandle

Do not reuse `FileHandle` in `file_io_sync`. Current `FileHandle` is an alias for `IoHandle` from `conflux.uring.handle`, and that target imports uring vocabulary and direct-slot semantics.

Add a minimal POSIX-only type:

```cpp
export class UniqueFd {
public:
    UniqueFd() noexcept;
    explicit UniqueFd(int fd) noexcept;
    ~UniqueFd() noexcept;
    UniqueFd(UniqueFd&&) noexcept;
    UniqueFd& operator=(UniqueFd&&) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] int release() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
};
```

Async `FileReader` can still use `FileHandle` internally.

## Updated helper set

Add to `conflux::file_io_sync`:

```cpp
export struct FileStat {
    uint64_t size{};
    uint64_t mtime_ns{};
    uint64_t ctime_ns{};
    uint64_t dev{};
    uint64_t ino{};
    uint32_t mode{};
};

export expected<UniqueFd, FileIoError> openat_contained_sync(
    int root_fd,
    string_view relative,
    int flags,
    mode_t mode = 0
) noexcept;

export expected<FileStat, FileIoError> stat_at_sync(
    int dir_fd,
    string_view path,
    int flags = 0,
    unsigned mask = STATX_BASIC_STATS
) noexcept;

export expected<FileStat, FileIoError> fstat_sync(int fd) noexcept;

export expected<void, FileIoError> write_all_fd(
    int fd,
    span<byte const> bytes
) noexcept;

export expected<string, FileIoError> read_text_file_sync(
    string_view path,
    size_t max_bytes = 16 * 1024 * 1024
);

export optional<string> read_text_file_nothrow(
    string_view path,
    size_t max_bytes = 16 * 1024 * 1024
) noexcept;
```

`write_all_fd` remains blocking-fd-only. On `EAGAIN`/`EWOULDBLOCK`, return `would_block`; do not spin.

## Updated LineRange placement

Place line-view helpers in `conflux.utils`, but keep them independent from file I/O:

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

## Updated eprint/eprintln implementation

Move diagnostics from `types.cxx` to `utils.cxx`.

Preferred once `file_io_sync` exists:

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

## Migration order

```text
0. Move/add core error type; add file_io_sync UniqueFd and POSIX helpers.
1. Add LineRange and trim/split helpers to utils.
2. Move eprint/eprintln to utils and remove stream exports from types.
3. Replace config_from_ini ifstream path.
4. Replace DNS tolerant parsers with read_text_file_nothrow.
5. Replace template load/reload whole-file reads.
6. Replace DB SQL file loader.
7. Replace HTTP fdinfo diagnostics.
8. Decide whether carrier_coro std::print(stderr, ...) stays as cold output or moves to eprintln.
9. Add scripts/check_no_std_streams.py gate after sources are clean.
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
```

Do not fail `std::format` in this pass. Treat it separately as cold-vs-hot diagnostic policy.
