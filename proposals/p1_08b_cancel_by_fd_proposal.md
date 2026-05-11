# P1-08b: Recv-Only Generation Invalidation and No-Stall Close

Date: 2026-05-10
Status: APPROVED
Effort: 1–2 days
Prerequisite: P1-08 (PR A + PR B complete)

## Problem

`cancel_recv_if_armed()` submits a cancel SQE for the multishot recv.
If the SQ is full, the cancel is deferred and `queue_close()` stalls —
the entire close path blocks on recv cancel success.

The generation check in `handle_recv_cqe` already filters stale CQEs
by comparing `conn.gen` against the CQE user_data gen. This means a
generation bump is sufficient to discard stale recv completions — the
cancel SQE is best-effort, not required for correctness.

## Constraint: `conn.gen` Is Not Recv-Only

`conn.gen` guards recv, send, close, websocket handoff, deferred waits,
and slot reuse. Bumping it indiscriminately breaks:

- **Close completion**: `conn_erase(fd, gen)` erases only if
  `conn.gen == gen`. A close CQE carrying a pre-bump generation is
  ignored → fd-table entry leaks → shutdown spins forever.
- **Send completion**: `handle_send()` ignores CQEs whose generation
  doesn't match. Bumping during in-flight send → send completion
  dropped → `send_queued` stays true → `close_after_send` never fires.
- **Incremental recv buffers**: partial buffers must be retired before
  the generation changes, or stale final CQEs won't reclaim them.

The fix must bump `conn.gen` **only to invalidate recv**, and only
after retiring any incremental partial buffer. It must also handle
the send-queued shutdown case where gen cannot be bumped.

## Design

### 1. `invalidate_recv_if_armed(fd)`

New helper. Retires incremental state, bumps gen, best-effort cancel.

Do not call from the shutdown wait-for-send path before the send CQE
has completed. Forced close/error paths may still call `queue_close()`
even if `send_queued` is true, because they no longer depend on that
send CQE for progress.

```cpp
void invalidate_recv_if_armed(int fd) {
    auto const ufd = static_cast<SZ>(fd);
    if (ufd >= fd_table.size())
        return;

    auto& conn = fd_table[ufd];
    if (conn.fd < 0 || !conn.recv_armed)
        return;

    u32 const old_gen = conn.gen;

    retire_incremental_partial(fd, old_gen, conn);

    ++conn.gen;              // invalidate old recv CQEs
    conn.recv_armed = false;

    auto handle = accepted_sockets_direct
        ? SocketHandle::from_direct(static_cast<u32>(fd))
        : SocketHandle::from_os(conn.fd);

    // Best-effort. SQ-full failure is fine — gen mismatch handles it.
    (void)submit_cancel_multishot_recv(raw_, handle, pack(Op::Nop, 0, 0));
}
```

### 2. `queue_close(fd)` — invalidate recv first, close at current gen

```cpp
void queue_close(int fd) {
    auto const ufd = static_cast<SZ>(fd);
    if (ufd >= fd_table.size())
        return;

    auto& conn = fd_table[ufd];
    if (conn.fd < 0 || conn.closing)
        return;

    invalidate_recv_if_armed(fd);   // bumps gen if recv was armed

    u32 const close_gen = conn.gen; // post-bump — matches conn_erase check

    // ... existing direct-vs-os close logic ...
    // submit close with pack(Op::Close, close_gen, fd)
}
```

Close submission may still defer if SQ is full. That is acceptable —
the fix is only that **recv cancel failure no longer blocks close**.

### 3. `handle_recv_cqe` and `phase1_copy_recv_bufs` — `close_after_send` discard guard

For send-queued connections where gen was not bumped, matching-gen recv
CQEs can still arrive. `recv_armed=false` alone is not a discard
guard — current `handle_recv_cqe` processes matching-gen CQEs into
`recvs` regardless of `recv_armed`. An explicit discard path is needed.

**In `handle_recv_cqe`** — early discard before existing logic:

```cpp
void handle_recv_cqe(int fd, int res, u32 flg, u32 gen) {
    auto const ufd = static_cast<SZ>(fd);
    if (ufd >= fd_table.size()) {
        discard_recv_bufs(res, flg);
        return;
    }

    auto& conn = fd_table[ufd];
    bool const gen_match = conn.gen == gen;
    bool const ws_pending = ws_cancel_handoffs.find(fd) != ws_cancel_handoffs.end();

    if (gen_match && conn.close_after_send) [[unlikely]] {
        if (!cqe_has_more(flg))
            conn.recv_armed = false;

        if (res <= 0 && !cqe_has_buffer(flg))
            reclaim_retired_incremental_recv(fd, gen);
        else if (cqe_has_buffer(flg))
            discard_recv_bufs(res, flg);

        return;
    }

    // ... existing logic unchanged ...
}
```

**In `phase1_copy_recv_bufs`** — same discard guard, after stale-gen
and websocket-pending checks, before `append_recv_buf_to(conn.partial)`:

CQ batch ordering means a recv CQE can be pushed into `recvs` while
`close_after_send` is still false, then shutdown sets it true before
`phase1_copy_recv_bufs` runs. Without this guard, that recv data would
be copied into `conn.partial`.

```cpp
auto& conn = fd_table[ufd];

if (conn.close_after_send) [[unlikely]] {
    u32 const orig_flags = rc.flags;

    discard_recv_bufs(rc);

    if (rc.res <= 0 && !cqe_has_buffer(orig_flags))
        reclaim_retired_incremental_recv(rc.fd, rc.gen);
    else if (rc.res > 0 && cqe_has_buffer(orig_flags))
        clear_retired_incremental_if_final(rc.fd, rc.gen, orig_flags);

    continue;
}
```

### 4. `handle_shutdown()` — no global gen bumps

```cpp
void handle_shutdown() {
    shutting_down = true;
    cancel_accept_or_defer();

    for (SZ i = 0; i < fd_table.size(); ++i) {
        auto& conn = fd_table[i];
        if (conn.fd < 0)
            continue;

        if (conn.sse_channel)
            conn.sse_channel->close();

        if (conn.send_queued) {
            // Do NOT bump conn.gen — let in-flight send complete.
            // Set close_after_send so handle_recv_cqe discards (§3).
            conn.close_after_send = true;

            // Best-effort cancel recv. Leave recv_armed true until
            // final cancel/recv CQE arrives — it means "kernel recv
            // may still be active," not "logically processing."
            if (conn.recv_armed) {
                auto handle = accepted_sockets_direct
                    ? SocketHandle::from_direct(static_cast<u32>(i))
                    : SocketHandle::from_os(conn.fd);
                (void)submit_cancel_multishot_recv(
                    raw_, handle, pack(Op::Nop, 0, 0));
            }
        } else {
            queue_close(static_cast<int>(i));
        }
    }
}
```

For send-queued connections: `close_after_send=true` activates the
discard guard in both `handle_recv_cqe` and `phase1_copy_recv_bufs`.
`recv_armed` stays true until the final CQE clears it. Send completion
proceeds normally → `handle_send_complete` → `queue_close()` →
`invalidate_recv_if_armed` → clean close.

### 5. Accept cancel — self-redeferring retry

```cpp
void cancel_accept_or_defer() {
    if (!submit_cancel_by_ud(raw_, pack(Op::Accept, 0, listen_fd), 0))
        defer_op([this] { cancel_accept_or_defer(); });
}
```

`submit_cancel_by_ud()` only returns false on SQE acquisition failure.
Self-redeferring retries until SQ has space.

## Changes

| File | Change |
|---|---|
| `http_server.cxx` | Add `invalidate_recv_if_armed()` helper |
| `http_server.cxx` | Add `cancel_accept_or_defer()` helper |
| `http_server.cxx` | `queue_close`: call `invalidate_recv_if_armed` instead of `cancel_recv_if_armed`; close at post-bump gen |
| `http_server.cxx` | `handle_recv_cqe`: add `close_after_send` discard guard with `[[unlikely]]` before existing logic |
| `http_server.cxx` | `phase1_copy_recv_bufs`: add `close_after_send` discard guard for CQ-batch-ordered recv data |
| `http_server.cxx` | `handle_shutdown`: use `cancel_accept_or_defer`; set `close_after_send` for send-queued; best-effort recv cancel without gen bump |

## Acceptance Criteria

1. `queue_close` never defers due to recv cancel SQE failure.
2. Close CQE carries the current `conn.gen` — `conn_erase` always matches.
3. Send-queued connections complete send → `close_after_send` → clean close.
4. No matching-gen recv CQE is processed into a new request after `close_after_send=true`, including recv data queued in `recvs` before `close_after_send` was set (CQ batch ordering).
5. Incremental partial buffers retired before gen bump.
6. Accept cancel retries until successful (not silently dropped).
7. `recv_armed` stays true for send-queued connections until final CQE clears it.
8. Shutdown drain completes without stalls under SQ pressure.
9. No CQE use-after-free or fd-table entry leak.
10. Existing tests pass (E2E HTTP tests, socket_task_ring tests).
11. No regression in `tcp_increment_coro_bench` (--compare-bins, release).

## Tests

### Stress tests

Extend existing PR-A shutdown tests:

- `ring_entries = 16`, 100 idle connections
- Mixed idle + large-response connections (send-queued at shutdown time)
- Accepted-socket direct mode (if supported)
- Incremental recv buffer mode
- Assert `srv.stop()` returns (no spin)
- Assert all client fds see EOF/RST
- Assert no recv data processed after `close_after_send` set

### Batch-order tests

1. **Recv-before-shutdown in same CQ batch**: recv data queued in
   `recvs` while `close_after_send=false`, shutdown sets it true before
   `phase1_copy_recv_bufs` — assert data discarded, not copied into
   `conn.partial`.

2. **Final recv CQE before send completion**: `close_after_send=true`,
   final recv CQE clears `recv_armed`, then send completes and
   `queue_close()` runs — assert shutdown finishes cleanly.

## Future: Split Generation Domains

The cleaner long-term design splits `conn.gen` into two domains:

```cpp
u32 conn_gen;   // fd slot / send / close / lifetime
u32 recv_gen;   // multishot recv only
```

This would let shutdown invalidate recv aggressively without risking
send/close/deferred/websocket state. With the current single `conn.gen`,
the "invalidate recv only" invariant must be maintained by discipline
and the `close_after_send` discard guard. Not in scope for P1-08b.

## Not in Scope

- Split `conn_gen` / `recv_gen` (future, see above).
- Per-fd cancel slot tracking (overkill with gen approach).
- `IORING_ASYNC_CANCEL_ANY` batch cancel (optimization, can add later).
- Close deferral elimination (close can still defer on SQ-full; only recv-cancel deferral is removed).
