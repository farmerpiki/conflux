# T1-A: Zero-Copy HTTP Response Send (SEND_ZC)

Date: 2026-05-10
Status: IN PROGRESS (steps 1–7 landed; steps 8–10 remain)
Effort: 5–8 days (3–5 plain responses, +2–3 mapped-file correctness)
Prerequisite: P2-G (registered send buffers) recommended but not required
Kernel: 6.0+ (`IORING_OP_SEND_ZC`)

## Problem

HTTP response bodies go through `io_uring_prep_send` / `io_uring_prep_writev`
today. The kernel copies the user buffer into kernel skb memory. For responses
≥ 1 KiB this copy is measurable under high throughput. `SEND_ZC` eliminates
the copy by pinning the user buffer until the NIC is done with it.

## Current State

### Primitives (done, in file_io.cxx)

```cpp
// file_io.cxx:2458 — resolves on first CQE only (unsafe: buffer reuse before notif)
Task<SZ> unsafe_send_zc_sent_async(FileHandle, void const*, SZ, int flags, unsigned zc_flags);

// file_io.cxx:2490 — properly waits for notification CQE (safe)
Task<SZ> send_zc_async(FileHandle, void const*, SZ, int flags, unsigned zc_flags);
```

### Completion (done, in uring_completion.cxx:92-102)

Dual-CQE protocol is handled:
1. First CQE (no NOTIF): send result, caches `zc_bytes`, sets `zc_seen_send`.
2. Second CQE (NOTIF): buffer-release signal, resolves task with cached bytes.

`has_pending_zc_notifications()` prevents cancel while waiting for NOTIF.

### Capability detection (done)

`IoUringCaps::send_zc` is probed via `io_uring_get_probe_ring()` at startup.

### HTTP response send path (not wired)

Three send paths, all using regular SEND/WRITEV:
- **Plain**: `queue_send()` → `submit_send_borrowed()` → `prep_send()`
- **Mapped file**: `queue_send_mapped()` → `submit_writev_borrowed()` → `prep_writev()`
  Note: `MappedFile` will become `MappedBody` (lease + offset + size) per mapped cache proposal; `SP<MappedFile>` becomes `SP<MappedBody>` in the response variant; `queue_send_mapped` will use `body.lease.bytes().subspan(body.offset, body.size)`.
- **Streamed file**: `queue_send_streamed()` → `submit_send_borrowed()` for headers,
  then `splice_to_fd()` for body (already zero-copy via splice)

## Design

### Op Tag

Add `Op::SendZc` to distinguish ZC completion from regular send:

```cpp
enum class Op : u8 { ..., SendZc, ... };
```

### Buffer Lifetime Contract

SEND_ZC requires the send buffer to remain valid until the notification CQE
arrives. Current `Conn::own_response` is a `std::string` — it's stable as
long as the Conn isn't reused. Since we don't reuse the Conn until send
completes (and handle_send clears own_response), the buffer is stable.

For mapped-file bodies, the mmap region is held by `Conn::mapped_file` —
also stable until send completes. No lifetime changes needed.

### Socket API

```cpp
// socket_io.cxx — new
bool submit_send_zc_borrowed(
    SocketRawRing& ring,
    SocketHandle handle,
    void const* data,
    SZ len,
    u64 user_data,
    int msg_flags = MSG_NOSIGNAL);
```

Calls `io_uring_prep_send_zc(sqe, fd, data, len, flags, 0)` with
`IORING_SEND_ZC_REPORT_USAGE` in ioprio.

### Server Integration

In `queue_send()`, capability-gated:

```cpp
if (caps.send_zc && len >= kSendZcThreshold) {
    if (submit_send_zc_borrowed(raw_, handle, data, len,
                                 pack(Op::SendZc, conn.gen, fd)))
        return;
}
// Fallback: regular send
submit_send_borrowed(raw_, handle, data, len, pack(Op::Send, conn.gen, fd));
```

`kSendZcThreshold`: minimum response size to use ZC (default 16384 bytes).
Below this threshold the extra CQE, pinning, and notification overhead
can erase gains. Let benchmarks lower it.

### Completion Handler

```cpp
enum class ZcAfterNotif : u8 {
    none,
    complete_response,
    resubmit_plain,
    resubmit_mapped,
    resubmit_streamed_header,
    close_after_error
};

// Per-connection ZC state
bool zc_waiting_notif = false;
ZcAfterNotif zc_after_notif = ZcAfterNotif::none;
bool zc_close_after_notif = false;  // close/timeout override — checked before zc_after_notif
```

On first CQE — check `F_MORE` before handling `res`, because NOTIF is
coming regardless of success/failure:

```cpp
if (flags & IORING_CQE_F_MORE) {
    conn.zc_waiting_notif = true;

    if (res < 0) {
        conn.zc_after_notif = ZcAfterNotif::close_after_error;
    } else {
        conn.written += static_cast<SZ>(res);
        if (conn.written >= total)
            conn.zc_after_notif = ZcAfterNotif::complete_response;
        else
            conn.zc_after_notif = ZcAfterNotif::resubmit_plain;
    }
    return;
}

if (res < 0) {
    fail_send(fd, conn);
    return;
}
conn.written += static_cast<SZ>(res);
// No MORE flag = no NOTIF expected, buffer reusable immediately
finish_plain_send(fd, conn);
```

On NOTIF CQE — close/timeout override checked first:

```cpp
conn.zc_waiting_notif = false;

if (conn.zc_close_after_notif) {
    conn.zc_close_after_notif = false;
    conn.zc_after_notif = ZcAfterNotif::none;
    clear_send_buffers(conn);
    queue_close(fd);
    return;
}

auto action = std::exchange(conn.zc_after_notif, ZcAfterNotif::none);

switch (action) {
case ZcAfterNotif::complete_response:        finish_plain_send(fd, conn); break;
case ZcAfterNotif::resubmit_plain:           queue_send(fd); break;
case ZcAfterNotif::resubmit_mapped:          queue_send_mapped(fd); break;
case ZcAfterNotif::resubmit_streamed_header: queue_send_streamed(fd); break;
case ZcAfterNotif::close_after_error:        fail_send(fd, conn); break;
default: break;
}
```

**Constraint:** only one `SEND_ZC` in flight per connection in v1.
```

### Mapped File Path

Note: the response body variant will be redesigned (BodyKind + BodyBox or equivalent type-erased dispatch) as part of the router split, but SEND_ZC completion machinery is independent of variant shape. `Conn::mapped_file` will become a lease held via `MappedBody`.

**v1: sequential one-segment-at-a-time SEND_ZC, no linked SQEs.**

Linked `IOSQE_IO_LINK` with `SEND_ZC` requires `MSG_WAITALL` — a short
send without `MSG_WAITALL` is not treated as error, breaking the link
chain silently. Too risky for first implementation.

Instead:
1. If `written < header.size()`, send remaining header via regular `send`
   (headers usually below threshold — no dual-CQE overhead for 200–800 bytes).
2. After header completion, send mapped body via `SEND_ZC` if body ≥ threshold,
   otherwise regular `send`.
3. One send operation in flight per connection.

Follow-up: linked SQEs or `IORING_SEND_VECTORIZED` (kernel ≥ 6.10)
after sequential path is proven correct.

### Streamed File Path

No change needed — splice is already zero-copy. Header sends stay as
regular `send` unless header size crosses threshold. Headers are usually
too small to justify ZC overhead.

### Close / Timeout Safety

`conn_erase()` clears `own_response` and resets `mapped_file`. Unsafe if
ZC NOTIF still pending → UAF.

Deferral at the **very top** of `queue_close()`, before `shutdown()`,
before `submit_close()`, before direct-slot close handling:

```cpp
if (conn.zc_waiting_notif) {
    conn.zc_close_after_notif = true;
    conn.closing = true;              // suppress new work
    cancel_recv_if_armed(fd, false);  // best effort; no socket close yet
    // do NOT submit socket close, shutdown, or direct-slot close
    // do NOT erase/clear response buffers
    return;
}
```

On NOTIF arrival → `zc_close_after_notif` checked first → clear buffers → close.

### Shared Send-Completion Helpers

Current `handle_send()` has cleanup embedded per path. Do not duplicate
for ZC. Refactor first:

```cpp
void finish_plain_send(int fd, Conn& conn);
void finish_mapped_send(int fd, Conn& conn);
void finish_streamed_header_send(int fd, Conn& conn);
void fail_send(int fd, Conn& conn);
```

Both `handle_send()` and `handle_send_zc()` call same helpers → no drift.

## Config

```
[io_uring]
send_zc = auto       # auto: use if caps.send_zc; off: never; on: force
send_zc_threshold = 16384  # minimum response bytes to use ZC (start high, benchmark lower)
send_zc_report_usage = true
```

## Acceptance Criteria

1. Plain responses above threshold use `SEND_ZC` only when config and caps allow.
2. Response buffer not cleared/reused until NOTIF arrives when first CQE had `IORING_CQE_F_MORE`.
3. Full-send-with-NOTIF completes HTTP state machine after NOTIF.
4. Partial-send-with-NOTIF resumes only after NOTIF in v1.
5. Close/timeout/shutdown while waiting for NOTIF does not free `own_response` or `mapped_file`.
6. Mapped-file path is sequential and ordered; no linked `SEND_ZC` unless `MSG_WAITALL` semantics implemented and tested.
7. `SEND_ZC_REPORT_USAGE` counters prove whether bytes were actually copied or zero-copied.
8. Benchmarks show no regression for sub-threshold responses and clear win for at least one realistic medium/large response class.

## Benchmark Gate

Matrix testing required — single-size wrk is insufficient:

```
body sizes:   512B, 1K, 4K, 16K, 64K, 256K, 1M
connections:  1, 16, 100, 1000
modes:        regular send, SEND_ZC auto, SEND_ZC forced
paths:        plain dynamic, mapped static, streamed static
transport:    loopback and real NIC if available
```

Must pass `--compare-bins` non-regression on release builds.

## Instrumentation Counters

`IORING_SEND_ZC_REPORT_USAGE` must feed counters — without them, may
benchmark "SEND_ZC enabled" while kernel/NIC mostly copies:

```
send_zc_attempts
send_zc_bytes_requested
send_zc_bytes_sent
send_zc_notifs
send_zc_copied_notifs
send_zc_errors_enomem
send_zc_errors_other
send_zc_fallback_regular_send
send_zc_no_notif
```

Note: `send_zc_no_notif` counts first CQEs without `F_MORE`. Do not
label as "kernel copied" unless usage reporting confirms — it means no
second CQE expected, buffer reusable immediately.

## Fallback Behavior

- `send_zc = auto`: on `-EOPNOTSUPP`, `-EINVAL`, or repeated copied
  notifications → disable ZC, retry regular send.
- `send_zc = on`: fail startup if capability missing, but still fall back
  on per-send `-ENOMEM` (low `ulimit -l` can cause ZC failure).

Autodisable hysteresis: after ≥ 1024 `SEND_ZC` attempts and ≥ 16 MiB
attempted bytes, if `copied_notifs / notifs > 0.90`, disable `SEND_ZC`
for this ring. Do not let loopback benchmarks permanently poison the
policy — virtual/loopback devices can report copied behavior because
zerocopy depends on NIC capabilities (scatter-gather TX, checksum offload).

## Implementation Order

1. Config and counters, including `send_zc_report_usage` and `send_zc_no_notif`.
2. `submit_send_zc_borrowed()` in `socket_io.cxx`; set report-usage via `sqe.ioprio(...)`.
3. `Op::SendZc`.
4. Refactor regular `handle_send()` cleanup into shared helpers (`finish_plain_send`, `finish_mapped_send`, `finish_streamed_header_send`, `fail_send`).
5. Per-connection ZC state.
6. Wire **plain non-TLS responses only**.
7. Close/timeout deferral at top of `queue_close()`.
8. Benchmark plain responses.
9. Mapped sequential path: regular-send headers, ZC body when body ≥ threshold.
10. Leave TLS and streamed-file body paths alone.

## Implementation Guardrails

### ZC deferred ops must capture generation

Deferred work after NOTIF should guard against close/reuse/gen transitions:

```cpp
auto gen = conn.gen;
defer_op([this, fd, gen] {
    auto ufd = static_cast<SZ>(fd);
    if (ufd >= fd_table.size() || fd_table[ufd].gen != gen)
        return;
    queue_send(fd);
});
```

Avoids rare SQE exhaustion → wrong-tenant send.

## Verdict

**Approved after three review rounds.** Implementable and worthwhile.

Round 1 corrections (structural):
- Completion state machine rewritten with `ZcAfterNotif` enum + deferred NOTIF-driven completion.
- `conn_erase()` UAF when NOTIF pending → deferred close.
- Linked mapped-file `SEND_ZC` unsafe without `MSG_WAITALL` → deferred to follow-up.
- Threshold raised 1 KiB → 16 KiB.
- Benchmark gate expanded to body-size × connection matrix.
- Instrumentation counters required.

Round 2 corrections (implementation-spec precision):
- Collapsed `zc_close_after_notif` / `close_conn` dual model → bool overrides enum.
- `res < 0` with `F_MORE` handled defensively (NOTIF still coming, must not free buffers).
- Close deferral moved to very top of `queue_close()`, before shutdown/submit_close.
- Shared helpers (`finish_plain_send`, `fail_send`, etc.) required before ZC wiring.
- Mapped headers stay regular `send` (below threshold), only body gets ZC.
- `send_zc_report_usage` added to config block.
- `send_zc_no_notif` counter added.
- Autodisable hysteresis defined: ≥ 1024 attempts + ≥ 16 MiB, > 90% copied → disable.

Round 3 (implementation guardrails, no design changes):
- Close deferral must suppress recv-side progress (`conn.closing = true`, cancel armed recv).
- ZC deferred ops capture generation to guard against close/reuse races.

## Not in Scope

- RECV_ZC (blocked on kernel ≥ 6.20, separate proposal).
- Registered buffer integration (P2-G handles that independently).
- SEND_VECTORIZED (can be added as follow-up when kernel support is confirmed).
