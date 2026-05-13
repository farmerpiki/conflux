# Proposal: Remove standard stream dependencies from reusable `conflux` sources

Status: **recommended, with narrower enforcement than the original draft**  
Scope: source-tree cleanup before first release, feature-boundary hygiene, and API direction  
Non-goal: claiming a major runtime win from removing streams alone

## Summary

Remove C++ iostream/fstream/sstream usage from reusable `conflux` library sources and replace it with explicit file, fd, buffer, and line-view primitives.

This should not become a `conflux::ostream` project. The goal is to avoid pulling locale-aware, stateful, allocation-prone stream machinery into low-level modules and to keep APIs aligned with the existing direction: `span`, `string_view`, `std::expected`, explicit ownership, explicit buffers, and no hidden global state.

The original idea is sound, but it needs two important corrections:

1. **Do not ban `std::println` everywhere.** It is fine in examples, tools, tests, CLI-style utilities, and cold human-facing output. What should go away is exporting `std::println`, `std::cerr`, or stream objects from foundational modules, and using stdout/stderr side effects in reusable internals where a fd/logger/buffer interface is clearer.
2. **Do not make sync file helpers depend on io_uring.** JSON, config parsing, template loading, and other small cold-path consumers should not be forced to link the io_uring compiled unit. HTTP server code already depends on io_uring, so using io_uring-backed helpers there is acceptable, but shared helpers used by JSON/config/utils must have a POSIX-only base path.
3. **Use the right prefix for the execution model.** Raw syscall-style helpers that can block the calling thread should move toward `blocking_*` public names. Executor-owned non-coroutine chains should use `sync_*`; coroutine APIs should use `async_*`. Existing `*_sync`/`*_async` names are transitional and will need a broader pre-v1 naming pass.

Because `conflux` has not been released yet, this is not a production compatibility migration. It is a chance to make the public and internal boundaries cleaner before users depend on them.

## Decision

Implement the stream cleanup, but frame it as **dependency-boundary cleanup**, not as a hot-path optimization.

Recommended policy:

- hard-ban iostream/fstream/sstream types from reusable library sources;
- allow `std::print`/`std::println` in examples, tests, benchmarks, tools, and intentionally human-facing programs;
- remove stream aliases from foundational exports (Phase A);
- decide separately whether `std::format` remains a global convenience alias or moves to cold helpers (Phase B — do not mix into the stream removal diff);
- add small POSIX fd/file helpers independent of io_uring; use `blocking_*` for raw syscall-style wrappers that may block the calling thread;
- keep io_uring-backed file/network work in the existing io_uring-dependent feature targets;
- enforce with a script once source-tree cleanup lands.

## Why this is worth doing

The direct runtime gain is likely small unless a specific stream call is in a hot loop. The real wins are different:

- cleaner feature split: JSON/config/template parsing can remain independent from io_uring;
- smaller dependency surface in low-level modules;
- fewer accidental locale/global-state interactions;
- less header/module baggage from exporting stream-related names;
- clearer APIs around ownership, buffers, and fallible operations;
- easier future enforcement of “hot paths use explicit buffers / `to_chars`, cold paths may format.”

This fits the broader design direction: own at the edges, view inside; return `expected` for fallible operations; avoid hidden globals; keep high-performance components modular instead of making every consumer pull the whole stack.

## Target policy

### Hard-banned in reusable library sources

Apply to `src/` modules and any library target intended to be linked by downstream users:

```text
<iostream>, <fstream>, <sstream>, <iosfwd>, <syncstream>
std::ifstream, std::ofstream, std::fstream
std::istream, std::ostream
std::stringstream, std::istringstream, std::ostringstream
std::istreambuf_iterator
std::cin, std::cout, std::cerr, std::clog
export using std::println
export using std::cerr
```

Rationale: these drag stream vocabulary into reusable components and encourage API designs based on global streams or stateful formatting.

### Allowed where appropriate

```text
std::print / std::println in examples, tests, benchmarks, tools, demos, CLI-style utilities
std::format in cold diagnostics/config/error-message paths
```

`std::println` is not the main problem. It is often the right ergonomic choice for simple human output. The problem is making stream/console output part of the reusable core API or using it where an fd, buffer, or logging callback should be explicit.

### Discouraged in hot paths

```text
std::format for per-request/per-token/per-field output
heap-building strings solely to write diagnostics
hidden stdout/stderr writes from reusable library internals
```

For hot paths, prefer `to_chars` and append into caller-owned buffers or internal fixed buffers.

## Feature-boundary model

The important design constraint is that small file/text helpers must not force users to compile or link io_uring.

Recommended target shape (aligned with [modular build targets proposal](modular_build_targets_proposal.md)):

```text
conflux::core              // types + utils (LineRange, trim/split helpers, eprint/eprintln). No liburing.
                           // eprint/eprintln live in utils partition, not types — types stays vocabulary-only.
conflux::file_io_sync      // POSIX fd/file helpers. No liburing.
                           // Raw syscall-style operations that may block should use `blocking_*` names as the API is cleaned up.
                           // Separate CMake target from conflux_file_io (which depends on uring/liburing).
conflux::file_io           // existing async/high-throughput file I/O, depends on runtime (liburing)
conflux::json              // parses caller-provided buffers/views, no file I/O dependency. Depends: core only.
conflux::json_file         // optional convenience adapter: read file + parse JSON. Depends: json + file_io_sync.
conflux::http_server       // depends on runtime + socket_io; may use file_io/file_io_sync freely
```

This introduces `file_io_sync` as a **separate CMake target**, not inside the existing `conflux_file_io` target (which already depends on conflux_uring, conflux_work, and PkgConfig::LIBURING). Placing it under `conflux_file_io` would defeat the dependency-boundary goal even if the code looks clean. The key is link dependency direction:

```text
conflux::core         <- conflux::file_io_sync <- conflux::file_io <- conflux::http_server
conflux::json         <- no file I/O dependency by default (depends: core only)
conflux::json_file    <- conflux::file_io_sync + conflux::json
```

`file_io_sync` is the target that enables `json` preset and `core` preset users to do cold-path file reads without pulling liburing. The modular build proposal's `json` preset (`core` + `BUILD_JSON`) can optionally add `json_file` for file convenience without escalating to `runtime`.

Do not make `conflux::json` depend on `file_io` or `file_io_sync`. JSON should primarily accept `string_view`, `span<byte const>`, mapped views, or caller-owned buffers. File-loading convenience belongs in the `json_file` adapter target.

## Current source-tree inventory

Known stream-like usage to remove from reusable sources:

| File | Current use | Recommended replacement |
|---|---|---|
| `src/types.cxx` | includes stream/console exports such as `std::println`, `std::cerr`, `eprintln()` via stream printing | stop exporting stream names; provide fd-backed `eprint/eprintln` only if needed in the foundation |
| `src/net/config.cxx` | `std::ifstream`, `std::getline` for INI/config | `read_text_file_sync()` + `LineRange` |
| `src/net/dns/dns.cxx` | `std::ifstream`, `std::getline` for `/etc/resolv.conf` / `/etc/hosts` | `read_text_file_nothrow()` + `LineRange` |
| `src/net/http_server.cxx` | `std::ifstream` for `/proc/self/fdinfo/<ring>` diagnostics | sync POSIX read helper is fine; io_uring-backed read is also fine because HTTP already depends on io_uring |
| `src/template.cxx` | `std::ifstream` + `istreambuf_iterator` for template load/reload | `read_text_file_sync()` |
| `src/work/carrier_coro.cxx` | `std::print(stderr, ...)` warning | either keep if this is intentionally cold human output, or replace with fd-backed `eprintln` to avoid stderr `FILE*` |
| `src/db/connection.cxx` | `ifstream` + `istreambuf_iterator` for SQL file loading | `read_text_file_sync()` |

Tests/examples/benchmarks can be cleaned after reusable sources. Since the project is unreleased, repo-wide cleanup can happen earlier than it would in a public compatibility migration, but `std::println` should remain allowed for human-facing output.

## Add POSIX sync helpers to `conflux::file_io_sync`

Add these to the `file_io_sync` target (POSIX-only, no liburing). During the later naming pass, direct syscall-style helpers that can block the caller should use `blocking_*` names; executor-owned chains should use `sync_*`:

```cpp
export expected<string, FileIoError> read_text_file_sync(
    string_view path,
    size_t max_bytes = 16 * 1024 * 1024
);

export expected<vector<byte>, FileIoError> read_binary_file_sync(
    string_view path,
    size_t max_bytes = 64 * 1024 * 1024
);

export expected<void, FileIoError> write_all_fd(
    int fd,
    span<byte const> bytes
) noexcept;

export expected<void, FileIoError> write_text_file_sync(
    string_view path,
    string_view text,
    mode_t mode = 0644
);

export expected<void, FileIoError> write_binary_file_sync(
    string_view path,
    span<byte const> bytes,
    mode_t mode = 0644
);
```

Useful overloads:

```cpp
export expected<void, FileIoError> write_all_fd(
    int fd,
    string_view text
) noexcept;

export optional<string> read_text_file_nothrow(
    string_view path,
    size_t max_bytes = 16 * 1024 * 1024
) noexcept;
```

The nothrow wrapper is only for intentionally tolerant file probes (DNS fallback, optional config). Name it to signal intent — callers should not reach for it as default error handling. `read_text_file_nothrow` is adequate; the `nothrow` suffix is a well-known C++ convention (cf. `std::nothrow`).

### Implementation notes

Use:

```text
openat/openat2 when available, with openat fallback
fstat/statx for reserve hints
read loop with EINTR handling
write loop with short-write + EINTR handling
explicit max_bytes cap
expected<T, FileIoError> as the primary low-level result
throwing wrappers only where existing APIs already throw
no FILE*
no iostreams
no locale-dependent parsing
```

v1 of `blocking_write_all_fd` is **blocking-fd only**. If the fd is nonblocking and returns `EAGAIN/EWOULDBLOCK`, return a `would_block` error immediately. Do not spin, do not retry. Nonblocking write loops belong in the io_uring/socket_io layer.

### Avoid overgeneralizing too early

Do not build a generic stream abstraction. Keep the first version boring:

- read whole small text file;
- read whole bounded binary file;
- write all bytes to fd;
- write whole file atomically enough for config/test use if needed later.

If JSON or HTTP later need high-throughput ingestion, they should use mmap, caller-provided buffers, or existing io_uring paths, not this cold-path helper.

## Add line parsing helpers to `conflux::core` (utils partition)

Add a small line-view utility to the utils partition of `conflux::core` that returns views into an existing buffer:

```cpp
export struct LineView {
    string_view text;
    size_t line_no;
};

export class LineRange {
public:
    explicit LineRange(string_view text) noexcept;
    iterator begin() const noexcept;
    iterator end() const noexcept;
};

export string_view strip_cr(string_view line) noexcept;
export string_view trim_ascii(string_view s) noexcept;
export optional<pair<string_view, string_view>> split_once(
    string_view s,
    char delimiter
) noexcept;
```

`LineRange` requirements:

```text
no allocation
preserve final line without trailing newline
handle empty input
handle trailing newline without inventing an extra content line
leave CR stripping to strip_cr(), or document if built in
line_no starts at 1
views remain tied to the owning text buffer
```

Use it for:

```text
INI/config parsing
/etc/resolv.conf
/etc/hosts
/proc/self/fdinfo diagnostics
template scanning/reload parsing
future small text config formats
```

This is a better primitive than repeatedly spelling `getline` semantics in each parser.

## Console and diagnostic output

The foundation should not export `std::println` or `std::cerr`.

Recommended minimal foundation API:

```cpp
export void eprint(string_view message) noexcept;
export void eprintln(string_view message) noexcept;
```

Implementation:

```text
use write_all_fd(STDERR_FILENO, ...)
never throw
avoid std::format inside the foundational module
append '\n' via small stack buffer or two writes
optionally serialize writes with a small mutex only if line atomicity matters
```

Put `eprint/eprintln` in the `utils` partition, not `types`. Current `types.cxx` exports `std::println`, `std::cerr`, `std::format`, and defines `eprintln()` using `std::println(std::cerr,...)` — this makes types more than vocabulary aliases. Moving fd-backed diagnostics to utils keeps types clean and avoids forcing `<iostream>` into the base module. Formatted diagnostics (`eprintln_fmt`) belong in a colder helper layer or at the call site.

Optional cold helper:

```cpp
template<class... Args>
void eprintln_fmt(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        eprintln(std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {
        eprintln("conflux: diagnostic formatting failed");
    }
}
```

This helper should not be exported from the deepest foundation if it pulls too much formatting machinery into all users.

### Where `std::println` remains fine

Keep `std::println` in:

```text
examples
benchmarks
tests that print summaries
standalone tools
manual debugging utilities
CLI/demo programs
```

Do not waste time replacing every benchmark `std::println` with fd writes unless the goal becomes strict repo-wide stream/print minimization. The high-value cleanup is removing stream vocabulary from reusable library modules.

## Migration plan

### Phase 1: Land primitives

**Coordination note:** This phase overlaps with [modular build targets proposal](modular_build_targets_proposal.md) Phase 1 (conditional dep discovery, extract targets). Creating the `conflux::file_io_sync` target can be part of either effort, but must land before Phase 2 stream removal begins. If modular build Phase 1 runs first, `file_io_sync` should be created there and this proposal consumes it. If this proposal runs first, create `file_io_sync` as a new CMake target and the modular build effort adopts it.

Implement (in order — LineRange first, file helpers second):

```text
conflux::core (utils): LineRange
conflux::core (utils): strip_cr / trim_ascii / split_once
conflux::core (utils): eprint / eprintln (moved from types.cxx)
conflux_file_io_sync (new CMake target, no liburing dep):
  read_text_file_sync
  read_binary_file_sync
  write_all_fd (blocking-fd only; returns would_block on EAGAIN)
  read_text_file_nothrow wrapper
```

Tests:

```text
read empty file
read file without trailing newline
read CRLF file
read file with embedded NUL through binary helper
read file exactly at max_bytes
read file above max_bytes returns explicit error
write_all_fd handles short writes
write_all_fd handles EINTR if practical to simulate
LineRange preserves final line without newline
LineRange handles empty input and trailing newline predictably
split_once reports missing delimiter explicitly
```

Partial-write testing can use a pipe or socketpair with constrained capacity. If reliable simulation becomes annoying, cover the retry logic with a small injected syscall shim in unit tests rather than overcomplicating production code.

### Phase 2: Remove streams from reusable sources

Rewrite internal sources with the new primitives:

```cpp
auto text = read_text_file_sync(path).value();
for (auto line : LineRange{text}) {
    auto s = trim_ascii(strip_cr(line.text));
    // parse
}
```

For intentionally tolerant file probes:

```cpp
if (auto text = read_text_file_nothrow(path)) {
    for (auto line : LineRange{*text}) {
        // parse optional source
    }
}
```

Recommended order:

1. remove stream exports from `types.cxx` (Phase A — `std::println`, `std::cerr`, `#include<iostream>`);
2. migrate config/DNS/template text loading;
3. migrate `db/connection.cxx` SQL file loading (`ifstream` + `istreambuf_iterator` → `read_text_file_sync`);
4. migrate `/proc/self/fdinfo` diagnostics;
5. decide whether `carrier_coro.cxx` warning is fine as `std::print(stderr, ...)` or should use `eprintln` for consistency.

Since this has not shipped, no compatibility shim is needed for removed stream aliases unless current internal code still imports them.

### Phase 3: Clean tests/examples/benchmarks selectively

Do this after reusable sources are clean.

Prioritize removing:

```text
std::ifstream/ofstream/fstream
std::stringstream/istringstream/ostringstream where used as parsers/builders
istreambuf_iterator whole-file reads
```

Do not prioritize removing:

```text
std::println for benchmark summaries
std::println in examples
std::println in tools
```

If tests need simple file helpers, either use `conflux::file_io_sync` directly or add `tests/support_file.*` wrappers:

```cpp
test_write_text(path, text);
test_read_text(path);
test_write_binary(path, bytes);
```

## Enforcement

Add `scripts/check_no_std_streams.py`.

Initial gate:

```text
Fail in reusable source targets:
  #include <iostream>
  #include <fstream>
  #include <sstream>
  #include <iosfwd>
  #include <syncstream>
  std::ifstream / std::ofstream / std::fstream
  std::istream / std::ostream
  std::stringstream / std::istringstream / std::ostringstream
  std::istreambuf_iterator
  std::cin / std::cout / std::cerr / std::clog
  export using std::println
  export using std::cerr

Warn outside reusable sources:
  fstream/sstream usage
  streambuf iterators
```

Allowed:

```text
std::print / std::println in examples, tests, benchmarks, tools
std::format in cold diagnostics/config paths
```

Later optional stricter mode:

```text
Fail repo-wide for fstream/sstream.
Still allow std::println in human-facing programs.
```

Use an allowlist only when a compatibility test genuinely needs stream APIs. Every allowlist entry should include a reason and an expiry/removal condition.

**Target classification:** The script should gate on directory convention: `src/` = library (fail on streams), `examples/` `tests/` `benchmarks/` `tools/` = allowed. When the modular build proposal lands `CONFLUX_BUILD_*` component flags, the script can optionally derive target classification from CMake metadata instead.

## JSON-specific guidance

JSON should not learn about file loading by default.

Recommended JSON layering:

```cpp
// conflux::json
expected<json::document, json::error> parse(span<byte const> input, parse_options opts = {});
expected<json::document, json::error> parse(string_view input, parse_options opts = {});

// conflux::json_file (optional adapter, depends: json + file_io_sync)
expected<json::document, json::error> parse_file_sync(string_view path, parse_file_options opts = {});
```

`conflux::json` should stay independent from:

```text
io_uring
filesystem walking
stdio/iostream
process-global logging
hidden global allocators
```

For high-throughput JSON ingestion, prefer caller-provided buffers, mmap views, or an explicit integration with the io_uring file target. Do not make every JSON user pay for the server/file-ring stack.

## HTTP-server-specific guidance

The HTTP server already depends on the io_uring core, so it is allowed to use io_uring-backed file/network helpers internally.

Still, for cold diagnostics such as `/proc/self/fdinfo/<ring>`, the POSIX sync helper is simpler and does not matter for hot-path performance. Either route is acceptable as long as the dependency does not flow backward into JSON/config/utils.

For static file serving or response send paths, keep using the existing io_uring machinery, registered buffers, splice/send-zc paths, and mmap helpers where they are already part of the HTTP feature.

## API examples

Config parsing:

```cpp
auto text = read_text_file_sync(path, 1 * 1024 * 1024);
if (!text) {
    return unexpected{text.error()};
}

for (auto line : LineRange{*text}) {
    auto s = trim_ascii(strip_cr(line.text));
    if (s.empty() || s.starts_with('#')) {
        continue;
    }

    auto kv = split_once(s, '=');
    if (!kv) {
        return unexpected{ConfigError::invalid_line(line.line_no)};
    }

    auto key = trim_ascii(kv->first);
    auto value = trim_ascii(kv->second);
    // parse key/value
}
```

DNS fallback parsing:

```cpp
if (auto text = read_text_file_nothrow("/etc/resolv.conf")) {
    for (auto line : LineRange{*text}) {
        auto s = trim_ascii(strip_cr(line.text));
        // parse nameserver/search/options lines
    }
}
```

Cold diagnostic:

```cpp
if (auto text = read_text_file_nothrow(fdinfo_path)) {
    for (auto line : LineRange{*text}) {
        if (line.text.starts_with("CqOverflowList:")) {
            eprintln(line.text);
        }
    }
}
```

Hot builder direction:

```cpp
char buf[64];
auto [ptr, ec] = std::to_chars(std::begin(buf), std::end(buf), value);
if (ec == std::errc{}) {
    out.append(buf, ptr);
}
```

Use this pattern later for JSON/HTTP builders where formatting is genuinely hot. Do not block the stream cleanup on rewriting every formatter.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Accidentally creating a new stream abstraction | Keep helpers fd/file/buffer-specific; no `operator<<`; no stream state object |
| Pulling `<format>` into the base module | Keep formatted diagnostics out of `types`; export only `eprint/eprintln(string_view)` if needed |
| Making JSON depend on io_uring | Keep `conflux::json` buffer-based; put file convenience in `conflux::json_file` adapter |
| Duplicating async file I/O with sync helpers | Document sync helpers as cold-path/startup/test utilities; keep high-throughput file work on existing io_uring APIs |
| Dangling `LineRange` views | Make ownership/lifetime explicit in docs and tests; never return `LineView`s beyond owner lifetime |
| Over-enforcing against useful `std::println` | Ban stream machinery in reusable sources, not human-facing printing in examples/tools/tests |
| Hiding file-size hazards | Keep `max_bytes` defaults conservative and require explicit opt-in for larger reads |
| Weak error surfaces | Use structured `FileIoError` with errno/op/path context where practical |

## Recommended implementation order

1. Add `LineRange`, `strip_cr`, `trim_ascii`, `split_once` to utils partition — mechanically smallest, gives immediate parser cleanup.
2. Create separate `conflux_file_io_sync` CMake target (POSIX-only, no liburing). Add/rename direct syscall-style helpers with explicit names such as `blocking_read_text_file`, `blocking_read_binary_file`, `blocking_write_all_fd`; reserve `sync_*` for executor-owned non-coroutine chains and keep nonblocking write loops in io_uring/socket targets.
3. Move `eprint/eprintln` from `types.cxx` to `utils.cxx`. Remove `export using std::println`, `export using std::cerr` from types. (Phase A — stream exports.)
4. Migrate config/DNS/template/fdinfo/db-connection source usage to new primitives.
5. Decide `carrier_coro.cxx` warning policy — keep `std::print(stderr,...)` if diagnostics layer adds unwanted baggage, or replace with `eprintln`.
6. Add CI stream check (`scripts/check_no_std_streams.py`) for reusable sources.
7. Separately decide `std::format` export fate (Phase B — do not combine with stream removal).
8. Migrate tests/examples/benchmarks opportunistically, preserving `std::println` where it is the clearest human-output API.

## Final recommendation

Implement this before release.

The best version of the change is not “ban all printing” and not “replace iostreams with a custom iostream.” It is:

```text
reusable internals: explicit fd/file/buffer/view APIs
cold diagnostics: format only where useful
human-facing tools/examples: std::println is fine
JSON/config/core: no io_uring dependency unless explicitly opting into an adapter
HTTP server: io_uring-dependent paths are fine because the feature already requires it
```

That gives the project cleaner build boundaries, better API hygiene, and fewer accidental heavy dependencies without pretending this is a large standalone runtime optimization.

## Verdict

**Accepted. Implement before first release.**

Classification: **API/dependency-boundary cleanup**, not a performance feature. Direct runtime gain small unless streams are on hot path. Real wins: cleaner feature split, smaller low-level surface, fewer hidden globals/locale state, clearer buffer/fd APIs.

### Code verification (2026-05-11)

Stream inventory verified against current tree. All proposal claims confirmed:

| File | Verified usage |
|------|---------------|
| `src/types.cxx` | `export using std::println` (L88), `export using std::cerr` (L89), `export using std::format` (L106), `#include<iostream>` (L2), `eprintln()` via `std::println(std::cerr,...)` with mutex (L124-129) |
| `src/net/config.cxx` | `std::ifstream` (L355), `std::getline` (L363) for INI parsing |
| `src/net/dns/dns.cxx` | `std::ifstream` + `std::getline` for resolv.conf (L740,744) and hosts (L777,781) |
| `src/net/http_server.cxx` | `std::ifstream` + `getline` for `/proc/self/fdinfo` (L3900) |
| `src/template.cxx` | `std::ifstream` + `istreambuf_iterator` for template loading (L1766,1769,1841,1844) |
| `src/work/carrier_coro.cxx` | `std::print(stderr,...)` for coro frame pool stats (L60) |
| **`src/db/connection.cxx`** | **MISSED by proposal.** `ifstream` + `istreambuf_iterator` for SQL file loading (L289,295) |

`conflux_file_io` confirmed to depend on `conflux_uring` + `conflux_work` + `PkgConfig::LIBURING` (all PUBLIC). This validates the core argument: sync file helpers must be a separate target.

### Key decisions applied

1. **`file_io_sync` = separate CMake target.** Not inside `conflux_file_io`. Otherwise dependency-boundary goal fails even if code looks correct.
2. **`eprint/eprintln` → `utils.cxx`, not `types.cxx`.** Types stays vocabulary-only. Current types.cxx has `#include<iostream>`, exports `std::println`/`std::cerr`/`std::format`, and defines `eprintln` — too heavy for a vocabulary module.
3. **`std::format` export = separate Phase B decision.** Removing streams (Phase A) and deciding `std::format` fate should not be combined — different diff, different tradeoff.
4. **`read_text_file_nothrow` naming kept.** `nothrow` suffix is well-known C++ convention (cf. `std::nothrow`). Intent is clear: tolerant probes only.
5. **`blocking_write_all_fd` v1 = blocking-fd only.** Returns `would_block` error on `EAGAIN/EWOULDBLOCK`. No spin. Nonblocking write loops belong in io_uring/socket_io.
6. **LineRange lands before file helpers.** Mechanically smaller, gives immediate parser cleanup for config/DNS/template/fdinfo/db.
7. **`db/connection.cxx` added to inventory.** SQL file loading uses `ifstream` + `istreambuf_iterator` — same pattern as template loading.
