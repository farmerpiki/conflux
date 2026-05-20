> Archived historical rationale. Current branch selection lives in `todo/proposal_state.md` and `todo/parallel_priority_plan.md`.

# Proposal: split `conflux.file_io` into buffer, pipe, reader, IOPOLL, and driver modules

## Recommendation

Implement this split before touching `conflux.work.root` or the remaining socket
surfaces. `src/file_io/file_io.cxx` is the best next large-module split because it
is large enough to hurt review/build ergonomics, but already has strong internal
section boundaries and a mostly non-template public surface.

This is worth implementing.

## Current state

`src/file_io/file_io.cxx` is about 3.2k LOC and currently exports five distinct
responsibilities from one module:

1. registered fixed-buffer table and fixed-buffer leases;
2. per-ring pipe-pair pool for splice / zero-copy chains;
3. general `FileReader` async SQE submission helpers;
4. dedicated storage-only IOPOLL reader/ring/pump helpers;
5. thread-local current-reader scope plus single-ring `pump_until` / `block_on`
   test/example driver helpers.

The seams are already visible in the file comments, so this split can be mostly
mechanical and does not require redesigning file APIs.

## Proposed module shape

Keep `conflux.file_io` as the compatibility umbrella for now, but move ownership
of narrower APIs into leaf modules:

| New module | Owns | Public dependency intent |
|---|---|---|
| `conflux.file_io.buffers` | `RegisteredBufferTable`, `FixedBuffer`, `FixedBufferPool` | `conflux.types`, `conflux.uring.handle`; no `conflux.work` |
| `conflux.file_io.pipe_pool` | `PipePair`, `PipePool` | `conflux.types`; no `conflux.work` |
| `conflux.file_io.reader` | `FileIoError`, `FileReader`, async path helpers | `conflux.work`, `conflux.uring`, `conflux.file_io_sync`, `buffers`, `pipe_pool` |
| `conflux.file_io.iopoll` | `IopollStorageRingOptions`, `IopollFileReader`, `IopollStorageRing`, IOPOLL decoders/pump helpers | `reader`, `buffers`, `conflux.uring.timeout` |
| `conflux.file_io.driver` | `current_file_reader`, `CurrentFileReaderScope`, default `pump_until`, `block_on` | `reader`, `conflux.uring.completion` |
| `conflux.file_io` | umbrella re-export | re-export all above plus `conflux.file_io_sync` during migration |

Do not create separate CMake package components in the first patch. Keep all new
module interface files inside the existing `conflux_file_io` target so component
selection and install shape remain stable while the source boundary is proven.
Component-level splits can follow once consumers import the leaf modules.

## Implementation order

1. Extract `buffers` and `pipe_pool` first. These are low-risk RAII utilities and
   immediately remove syscall-heavy code from the `FileReader` review surface.
2. Extract `reader` next. Preserve all current `FileReader` method names and the
   existing async/blocking alias policy.
3. Extract `iopoll` after `reader`, because it depends on fixed buffers and the
   general completion bridge conventions but is conceptually storage-only.
4. Extract `driver` last. It is useful for examples/tests, but should remain a
   helper layer rather than something core HTTP/runtime code must import.
5. Leave `conflux.file_io` as a thin umbrella that `export import`s the leaf
   modules. Remove only after downstream examples/tests have moved to direct
   imports.

## Ergonomics impact

- Users that only need fixed-buffer or pipe-pool types can import a small module
  without pulling `Task<T>`, `FileReader`, or the single-ring driver helpers.
- `FileReader` becomes the obvious home for async file operations rather than a
  catch-all for unrelated ring resources and example driver code.
- IOPOLL storage code gets its own module name, matching its deliberately narrow
  contract and making misuse harder in reviews.
- Tests can target `buffers`, `pipe_pool`, and `iopoll` directly instead of using
  the broad umbrella import.

## Performance / build impact

Expected runtime behavior is unchanged. The split should improve build and edit
latency by shrinking the transitive module surface for consumers that do not need
`FileReader` or `conflux.work`. It should also reduce accidental rebuilds when
IOPOLL or driver-helper code changes.

Hot-path constraints for the implementation branch:

- keep `FixedBuffer` / `PipePair` move/destructor paths inline and allocation-free;
- keep `FileReader` SQE submission behavior unchanged: methods enqueue SQEs but
  do not call `io_uring_submit()`;
- keep SQ-full behavior unchanged (`ENOSPC` task failure or boolean failure,
  depending on existing API);
- do not add virtual dispatch, shared ownership, or locks to buffer/pipe leases;
- do not move HTTP-server-specific ring driving into `conflux.file_io.driver`.

## Main risks

- C++20 module cycles: avoid by making `reader` depend on `buffers`/`pipe_pool`,
  never the reverse.
- Include drift: each leaf should own only the POSIX/liburing headers it needs;
  avoid copying the current broad global-module fragment everywhere.
- Public import churn: keep the umbrella until examples and tests have adopted
  narrower imports.
- Naming confusion with `file_io_sync`: keep `file_io_sync` as the POSIX blocking
  layer; do not move async `FileReader` helpers into it.

## Acceptance checklist

- `src/file_io/file_io.cxx` becomes a thin umbrella with no syscall-heavy bodies.
- New module interfaces compile inside the existing `conflux_file_io` target.
- Existing tests/examples pass with the umbrella unchanged.
- At least one test or example imports a leaf module directly to prove the split.
- `docs/component-map.md` continues to show one `file_io` component until a later
  package-component split is intentionally proposed.
