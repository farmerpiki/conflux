# Migration Guide — file_io/

Tracks completed callsite migration for `src/file_io/` as part of E1.2/E1.3.

## Inventory

No pending `src/file_io/` migration inventory remains. The historical `Flow<T>`
comment cleanup landed in E1.3, and current source no longer contains that
reference.

## Before / After pairs

_Populated as E1.2 PRs land._

## Blocking helper aliases

`conflux.file_io_sync` and `conflux.file_map` now export prefix-style
`blocking_*` helper names for direct caller-thread POSIX I/O and mmap setup:

- `blocking_open_tmpfile`
- `blocking_publish_tmpfile`
- `blocking_write_all_fd`
- `blocking_read_all_fd`
- `blocking_write_file_atomic_at`
- `blocking_write_text_file_atomic_at`
- `blocking_fstat`
- `blocking_stat_at`
- `blocking_read_file_at`
- `blocking_map_fd_readonly`
- `blocking_map_file_readonly`

The existing `*_sync` and unprefixed fd helper names remain available as
pre-release compatibility aliases. New file I/O examples and tests should prefer
the `blocking_*` names when the operation directly blocks the calling thread.

`conflux.json.file` follows the same rule for file-backed parsing via
`blocking_parse_file_at` and `blocking_parse_file`; pure JSON parsing APIs stay
unprefixed because they do not perform I/O.

## Async module split

`conflux.file_io` is now a compatibility umbrella over narrower leaf modules:

- `conflux.file_io.buffers` owns registered fixed-buffer table/pool leases.
- `conflux.file_io.pipe_pool` owns splice pipe-pair pooling.
- `conflux.file_io.reader` owns `FileReader` and async file-operation helpers.
- `conflux.file_io.iopoll` owns storage-only IOPOLL reader/ring helpers.
- `conflux.file_io.driver` owns thread-local current-reader scope plus test/example `pump_until` / `block_on` helpers.

Existing code can continue to import `conflux.file_io`. New focused tests, examples, and internal callers should import the narrow leaf module they use while the package still exposes one `conflux::file_io` component.
