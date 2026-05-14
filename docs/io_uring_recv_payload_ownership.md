# io_uring recv payload ownership

`RecvPayload` is the socket recv ownership boundary used by HTTP/server recv
code. It represents the bytes reported by one recv CQE and hides the concrete
buffer backend behind a small descriptor:

- `RecvPayloadStorage::provided_buffer_ring` — current implementation: bytes are
  borrowed from `BufferRing` slots selected by the kernel.
- `RecvPayloadStorage::recv_zc_reserved` — reserved shape for a later
  `IORING_OP_RECV_ZC` backend; no production SQE path uses it yet.
- `RecvPayloadPinning::kernel_buffer_ring_slot` — current lifetime: caller must
  finish copying/consuming bytes before returning the slot to the kernel.
- `RecvPayloadPinning::user_dma_pinned_buffer` — reserved future lifetime for
  caller-owned buffers pinned for zero-copy receive.

The HTTP path must decode CQEs through `try_recv_payload_from_cqe(...)` instead
of branching on `BufferRingMode` directly. This keeps the current classic,
recv-bundle, and incremental provided-buffer behavior unchanged while making the
future RECV_ZC branch a backend addition rather than another HTTP recv rewrite.

Current behavior:

- classic provided buffers expose one chunk per CQE;
- recv-bundle exposes one chunk per consumed buffer-ring slot;
- incremental buffers expose one chunk per CQE, and partial `BUF_MORE` CQEs keep
  the slot pinned until the final CQE arrives;
- `RecvPayload` recycles final/owned buffers in its destructor, so exception
  paths do not leak buffer-ring slots;
- callers can still call `recycle_all()` explicitly after copying bytes to make
  ownership release obvious in hot paths.

Do not implement RECV_ZC by mutating `BufferRing` internals. Add a new payload
backend with an explicit descriptor, pin/unpin lifecycle, feature probe, and
fallback path.
