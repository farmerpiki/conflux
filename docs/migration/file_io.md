# Migration Guide — file_io/

Tracks callsite migration for `src/file_io/` as part of E1.2.

## Inventory

### `src/file_io/file_io.cxx` — comment (1 occurrence)

| Line | Before | After |
|------|--------|-------|
| 530 | Comment mentioning `Flow<T> pipelines` | Update wording to `Task<T>` / `Chain<T>` |

Lands in E1.3 comment-cleanup pass.

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
