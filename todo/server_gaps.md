# Server / Framework Gaps

Status: open TODO

This file tracks only remaining gaps that still affect release confidence or
future branches.

## Open

- Deferred: streaming multipart upload API. Raw `http::UploadBody` is
  implemented for application-visible bounded streaming with backpressure and
  `save_to()`. Buffered multipart parsing and `http::Multipart` extractors are
  implemented today; `http::MultipartUpload` remains deferred until a caller
  needs streaming multipart events. Large multipart uploads must not be solved
  by raising `max_body_size` toward unbounded sizes.

## Verified Done

- Ring-thread `sched_setaffinity` and `IORING_REGISTER_IOWQ_AFF` application are
  implemented and wired from `Config::ring_core` / `worker_core_base`. If
  affinity becomes a release gate, add syscall injection or observable
  thread/affinity capture as test coverage rather than treating application as
  missing.
- HTTP handler execution docs already match code: handlers run on ring threads
  unless users explicitly offload work.
- HTTP request upload wire framing is covered for HTTP/1 chunked bodies,
  HTTP/2 DATA frames, and HTTP/3 DATA frames. The server stores/exposes decoded
  payload bytes rather than retaining encoded upload framing, with regression
  coverage for tiny chunk/DATA-frame uploads and HTTP/3 body-limit rejection.
- Raw `http::UploadBody` streaming is implemented for HTTP/1 and HTTP/2 routes,
  including bounded reads, error-aware EOF, early-return cancellation,
  `Expect: 100-continue`, `discard()`, `save_to()`, and upload metrics.
