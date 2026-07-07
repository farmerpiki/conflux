# Conflux Async File I/O API Reference

- **Module:** `conflux.file_io`
- **Primary leaf module:** `conflux.file_io.reader`
- **Namespace:** `conflux::file_io`
- **Component:** `conflux::file_io`
- **Backend:** caller-owned `io_uring` plus `CompletionTable`

`FileReader` is the lower-level async file, socket, and control-SQE surface. It
is a complete/advanced API, not the first-contact file API. Liburing-free
consumers should use `conflux.file_io_sync` instead.

## `FileReader`

```cpp
class FileReader {
public:
    root::Task<FileHandle> async_open(int dir_fd, std::string path,
                                      int flags, mode_t mode = 0);
    root::JoinTask<std::size_t> read_into(FileHandle const& fh,
                                          std::uint64_t offset,
                                          std::span<std::byte> out);
    root::JoinTask<std::size_t> write_into(FileHandle const& fh,
                                           std::uint64_t offset,
                                           std::span<std::byte const> data);

    root::Task<std::size_t> async_send_zc(FileHandle const& fh,
                                          void const* buf,
                                          std::size_t len,
                                          int flags = 0,
                                          unsigned zc_flags = 0);
};
```

The caller owns the ring, completion table, handles, and buffers. Buffers passed
to read/write/socket operations must remain valid until the returned task
resolves. Borrowed-buffer read/write operations return `JoinTask`, so dropping a
live operation terminates instead of silently detaching. `async_send_zc(...)`
waits for the zero-copy notification before the
task resolves; `async_unsafe_send_zc_sent(...)` resolves on the first send CQE
and requires the caller to keep storage alive by other means.

## Cancellation And Timeouts

Async file and socket operations are best effort to cancel after submission.
`FileReader::async_cancel(...)` and `async_cancel_fd(...)` request kernel
cancellation; a matching CQE may still arrive and the owning task/result decides
whether it is still live. Cancelling before an operation is admitted prevents
future submission.

Timeouts are explicit operations such as `async_timeout(...)`,
`async_link_timeout(...)`, `async_timeout_remove(...)`, and
`async_timeout_update(...)`, or caller-owned deadline/race logic around the
returned task. Ordinary file reads do not carry a hidden per-read deadline.
