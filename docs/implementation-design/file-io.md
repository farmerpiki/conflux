# `conflux.file_io` Design

Standalone library for uring-native zero-copy async file I/O. Sibling to
`conflux.work`, not part of `conflux.net`. Consumable from any subsystem that
owns an `io_uring *`.

This layer assumes the host satisfies the project-wide `io_uring` requirement.
`liburing` is mandatory at configure time, and `io_uring_queue_init*` must work
at runtime. See the top-level `README.md` for the supported platform contract.

## Summary

A thin layer over io_uring's file opcodes that returns `Flow<T>` for each
operation so callers can compose with `conflux.work` pipe operators
(`then`, `flat_then`, `on_error`). The library never submits; the owner's
run loop flushes the SQ and forwards CQEs into a library-owned
`CompletionTable`.

## Runtime Pieces

- `CompletionTable`: slot-indexed store of pending CQE callbacks. Each slot
  carries a generation counter; stale CQEs (slot reused after cancellation)
  are rejected. Not thread-safe — pinned to the ring's SINGLE_ISSUER thread.
- `FileHandle`: RAII owner of either a plain fd or an io_uring direct slot.
- `FixedBufferPool` / `FixedBuffer`: page-aligned slabs registered via
  `io_uring_register_buffers_sparse` + `_update_tag`. RAII lease returns the
  slot on drop.
- `PipePool` / `PipePair`: per-ring cache of `pipe2(O_DIRECT | O_CLOEXEC)`
  pairs for zero-copy splice chains.
- `FileReader`: the public submission surface — one method per op, each
  returning `Flow<T>`.

## Public API

```cpp
import conflux.file_io;

FileReader r{&ring, &completions, encoder};

Flow<FileHandle>           r.open_async(dir_fd, path, flags, mode);
Flow<FileHandle>           r.open_direct_async(dir_fd, path, flags, mode, file_index);
Flow<FileStat>             r.stat_async(handle);
Flow<FileStat>             r.statx_async(dir_fd, path, flags, mask);
Flow<size_t>               r.read_into(handle, off, span);
Flow<ReadFixedResult>      r.read_fixed(handle, off, FixedBuffer, max_bytes = npos);
Flow<size_t>               r.write_into(handle, off, span);
Flow<void>                 r.fsync_async(handle, data_only=false);
Flow<void>                 r.fallocate_async(handle, mode, off, len);
Flow<void>                 r.close_async(FileHandle);
Flow<size_t>               r.splice_to_fd(handle, off, len, dst_fd, PipePair);
```

## Decoupling: UserDataFn encoder + CompletionTable

The caller owns the full 64-bit `user_data` layout on its ring. The library
only needs to address its own completion slots. Each submission is tagged via
a caller-provided encoder:

```cpp
using UserDataFn = function<uint64_t(uint32_t slot, uint32_t gen)>;
```

HTTP server packs `(Op::FileIo, gen, slot)` through its existing
`pack(op, gen, fd)` helper, reusing the 8+24+32 bit layout. The library never
learns about `Op`. A caller with its own op space can embed different bits in
the high byte; the slot/gen only need 48 bits.

The owner's `dispatch_cqe` recognises library CQEs (its `Op::FileIo` case) and
forwards to `CompletionTable::dispatch(slot, gen, res, flags)`. Stale gen
rejects silently.

## Thread-local `current_file_reader()`

The router sits above any specific ring. Static-file handlers need to decide
between the async uring path and a synchronous fallback without holding a
pointer to the owning ring's `FileReader`. The library installs a thread-local
`FileReader *` via `CurrentFileReaderScope` at `run_loop` entry; handlers call
`current_file_reader()` to opportunistically pick the async path.

Handlers running off the ring thread (WorkPool fallback) see `nullptr` and
fall back to `mmap` + `writev`.

## Zero-copy paths

Plain HTTP — `splice_to_fd`:
- Two linked SQEs per chunk: splice file → pipe, splice pipe → socket, both
  with `SPLICE_F_MOVE | SPLICE_F_MORE` and `IOSQE_IO_LINK`.
- Chunk size = `pipe_capacity`. Short splice is re-driven inside the library.
- The `PipePair` is borrowed for the whole chain and dropped when the Flow
  resolves.

TLS — `read_fixed`:
- Acquire a `FixedBuffer` from the per-ring pool.
- `IORING_OP_READ_FIXED` fills the pre-registered buffer. No user-space copy
  into a staging vector.
- `SSL_write` into the wbio, `tls_flush_wbio`, `prep_send`.
- Pipeline depth 2 per connection.

## Fallbacks

- `FixedBufferPool` ctor tolerates `register_buffers_sparse` failure (older
  kernels, RLIMIT_MEMLOCK exhaustion). `pool.ok() == false`; `try_acquire`
  returns `nullopt`. The HTTP ring installs no `FileReader` unless both fixed
  buffers and splice pipes are usable, so static files fall back to mmap.
- `PipePool` tolerates `pipe2(O_DIRECT)` failure and retries without
  `O_DIRECT`.
- `FileReader` never calls `io_uring_submit()`; on `get_sqe() == nullptr`,
  the Flow rejects immediately with `FileIoError{ENOSPC, ...}`.

## Error Model

Negative `cqe->res` is surfaced as `FileIoError` (subclass of
`std::system_error`). The error carries `errno = -res`; callers use
`on_error` to convert into HTTP 500 / 404 as appropriate.

## Buffer / Pipe Sizing

Defaults:

| Knob                    | Default     | Per ring       |
|-------------------------|-------------|----------------|
| `fixed_buffer_slabs`    | 16          | × slab_bytes   |
| `fixed_buffer_bytes`    | 16 KiB      | page-aligned   |
| `splice_pipe_pairs`     | 4           | kernel default |

`RLIMIT_MEMLOCK` counts the registered-buffer pages. Containers with tight
memlock (default 8 MiB on many distros) should lower `fixed_buffer_slabs`, or
set it to 0 to disable the fixed-buffer path entirely. Shared HTTP tests opt
out via `mw_config()` so the TestServerRegistry doesn't exhaust memlock.

## Out of scope

- kTLS fast path: implemented via `Config::ktls` (`SSL_OP_ENABLE_KTLS`). When
  active post-handshake, `conn.ktls_send = true` and `start_streamed_body`
  (splice) replaces `start_streamed_tls_chunk` for TLS file responses.
