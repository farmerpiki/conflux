# Recv Bundle Proposal (P0-01)

Status: proposal (amended post-review x3)
Date: 2026-05-09

## Problem

Recv bundle is gated behind `IORING_FEAT_RECVSEND_BUNDLE` and disabled by
default, but it is enabled at runtime when the config requests it and the
kernel supports it. The current bundle path is incorrect in two independent
ways.

### Bug 1: server ad-hoc walk uses (buf_id + 1) % count

`http_server.cxx:2915-2926` walks bundle buffers using:

```cpp
u16 cur_buf = rc.buf_id;
while (remaining > 0) {
    ...
    cur_buf = static_cast<u16>((cur_buf + 1U) % bcount);
}
```

This assumes the kernel fills consecutive buffer IDs. It does not. The kernel
fills consecutive positions in the ring, and buffer IDs at those positions
depend on the recycle order. After any non-trivial recycling, IDs at
consecutive ring positions are not sequential. The liburing manual says the CQE
buffer ID is only the **first** buffer; consumed buffers are contiguous "in the
order in which they appear in the buffer ring," and apps must keep a cached head
index because the CQE does not contain the buffer-ring position.

### Bug 2: consume() is never called; head_pos_ is never advanced

`BufferRing` has a parallel position-tracking system: `ring_order_[]`,
`head_pos_`, and `consume()`. This is the correct way to map from ring
positions to buffer IDs for both classic and bundle paths.

`consume(n)` advances `head_pos_` by n and returns the start position.
`ring_id_at(pos)` returns `ring_order_[pos % count_]` — the buffer ID at that
ring position.

The server never calls `consume()`. It reads `rc.buf_id` from the CQE directly
and calls `return_buffer(id)` = `recycle(id)` without updating `head_pos_`.
This means `ring_order_[head_pos_ % count_]` is never the CQE buffer ID, and
any head-tracking invariant check will always fire.

`RecvSlices` and `buffer_slices_from_cqe` already exist as the correct decode
path but the server does not use them — the two tracking systems are
permanently desynced.

### Summary

| Path | Correct? |
|---|---|
| `buffer_slices_from_cqe` + `RecvSlices` (classic, n=1) | yes, but server never uses it |
| `buffer_slices_from_cqe` + `RecvSlices` (bundle, n>1) | yes, but server never uses it |
| Server direct `buf_id` + `return_buffer` (classic) | accidentally correct: one buffer, any ID |
| Server `(buf_id+1)%count` walk (bundle) | wrong: ID arithmetic instead of ring_order |

The server classic path works today only because with a single-buffer CQE the
wrong buffer-ID walk happens to read the right buffer. The bundle path is broken.

## Non-Goals

- Do not add `incremental` buffer ring mode here (that is P1-05 / `IOU_PBUF_RING_INC`).
- Do not change `RecvBuffer` or single-buffer `RecvBuffer::lease()` paths.
- Do not change the kernel buffer ring setup flags (`IOU_PBUF_RING_*`).

## Pre-implementation prerequisite: declaration order in `socket_io.cxx`

`buffer_slices_from_cqe` is currently defined before `cqe_has_buffer()` and
`cqe_buffer_id()` in the file. The amended body calls both helpers. Before
touching `buffer_slices_from_cqe`:

- Move `cqe_has_buffer()` and `cqe_buffer_id()` above `buffer_slices_from_cqe`, **or**
- Add forward declarations.

This is mechanical but required for the file to compile.

## Changes Required

### `src/net/http_server.cxx` — `RecvComp`

Change `buf_id` to `flags`. Remove the pre-decoded field; all decoding happens
at use time via `cqe_buffer_id(flags)`.

**Before:**

```cpp
struct RecvComp {
    int fd;
    int res;
    u32 gen;
    u16 buf_id;
};
```

**After:**

```cpp
struct RecvComp {
    int fd;
    int res;
    u32 gen;
    u32 flags;
};
```

In `handle_recv_cqe()`, store `cqe->flags` instead of
`cqe_buffer_id(cqe->flags)`. `buf_id` extraction deferred to
`buffer_slices_from_cqe` and `discard_recv_bufs`.

### `src/socket_io/socket_io.cxx` — `buffer_slices_from_cqe`

Change signature from `(BufferRing&, Cqe, bool bundle)` to
`(BufferRing&, int res, u32 flags, bool bundle)`. Do not pass full `Cqe`; the
server stores `res` and `flags` separately, and `Cqe` includes `user_data`
that duplicates already-unpacked `fd/gen`.

Add invariant assert before `consume()`:

```cpp
export [[nodiscard]] RecvSlices buffer_slices_from_cqe(
    BufferRing& ring,
    int res,
    u32 flags,
    bool bundle) noexcept {
    if (res <= 0 || !cqe_has_buffer(flags)) {
        assert(!cqe_has_buffer(flags));  // res <= 0 must not carry a buffer
        return {};
    }
    SZ const total = static_cast<SZ>(res);
    u32 const cnt = bundle
        ? static_cast<u32>((total + ring.buf_size() - 1) / ring.buf_size())
        : 1u;
    assert(cnt > 0);
    assert(cnt <= ring.count());
    u16 const first_id = cqe_buffer_id(flags);
    assert(ring.ring_id_at(ring.debug_head_pos()) == first_id);
    u32 const start = ring.consume(cnt);
    return RecvSlices{&ring, start, cnt, total};
}
```

The existing `cqe_buffer_id()` and `cqe_has_buffer()` free functions already
exist in `socket_io.cxx`; reuse them.

### `src/socket_io/socket_io.cxx` — `BufferRing` accessor

Add a debug-only accessor. Do not expose `head_pos_` as a general public API;
callers must not manage it directly.

```cpp
[[nodiscard]] u32 debug_head_pos() const noexcept { return head_pos_; }
```

Used only from `buffer_slices_from_cqe` (same module) for the invariant assert.
If the project has a module-private convention, prefer that over a public method.

### `src/net/http_server.cxx` — replace ad-hoc bundle loop

Replace `append_recv_buf_to` with `RecvSlices`:

**Before (current):**

```cpp
if (use_recv_bundle) {
    SZ remaining = static_cast<SZ>(rc.res);
    u16 cur_buf = rc.buf_id;
    u32 const bcount = buf_ring_->count();
    while (remaining > 0) {
        SZ const chunk = min(remaining, buf_ring_->buf_size());
        auto const bv = buf_ring_->buffer_view(cur_buf, chunk);
        dst.append(...);
        return_buffer(cur_buf);
        remaining -= chunk;
        cur_buf = static_cast<u16>((cur_buf + 1U) % bcount);
    }
} else {
    auto const bv = buf_ring_->buffer_view(rc.buf_id, static_cast<SZ>(rc.res));
    dst.append(...);
    return_buffer(rc.buf_id);
}
```

**After:**

```cpp
auto slices = buffer_slices_from_cqe(*buf_ring_, rc.res, rc.flags, use_recv_bundle);
auto recycle = scope_exit([&] noexcept {
    slices.recycle_all();
    rc.flags = 0;
});
for (auto const& s : slices)
    dst.append(reinterpret_cast<char const*>(s.bytes.data()), s.bytes.size());
```

`scope_exit` guarantees `recycle_all()` and `rc.flags = 0` both run even if `dst.append()` throws. The guard owns both actions so a throw cannot leave `rc.flags` nonzero while buffers are already recycled.
If the project builds with `-fno-exceptions` and this is enforced in CI, the
scope guard can be removed and a comment added; otherwise the guard is
mandatory. Do not rely on documentation of no-exception intent.

`RecvSlices::recycle_all()` calls `recycle_range()` which is consistent with
the `head_pos_` tracking.

### `src/net/http_server.cxx` — error path discard helper

Replace all direct `return_buffer(rc.buf_id)` recv-buffer drops with a single
helper that routes through the same consume/recycle path. A bundle error-path
CQE can span multiple ring positions; discarding as single-buffer leaks slots.

Provide two overloads: one for pre-`RecvComp` early returns in
`handle_recv_cqe()` where `res`/`flags` are still raw CQE values, and one for
later `RecvComp`-based paths.

```cpp
void discard_recv_bufs(int res, u32 flags) noexcept {
    if (!cqe_has_buffer(flags))
        return;
    auto slices = buffer_slices_from_cqe(*buf_ring_, res, flags, use_recv_bundle);
    slices.recycle_all();
}

void discard_recv_bufs(RecvComp& rc) noexcept {
    discard_recv_bufs(rc.res, rc.flags);
    rc.flags = 0;
}
```

Do not add `consume_and_recycle(u16 id)` to `BufferRing`; it is unsafe for
bundle paths where the CQE consumed multiple positions.

#### Early returns in `handle_recv_cqe()` must discard

Current `handle_recv_cqe()` has early returns before `RecvComp` is pushed:

```cpp
if (ufd >= fd_table.size())
    return;

if (!gen_match && !ws_pending)
    return;
```

If the CQE carries `IORING_CQE_F_BUFFER`, the kernel has already consumed a
provided buffer. Returning here without settle/recycle leaks the ring slot.
Fix:

```cpp
if (ufd >= fd_table.size()) {
    discard_recv_bufs(res, flg);
    return;
}

if (!gen_match && !ws_pending) {
    discard_recv_bufs(res, flg);
    return;
}
```

Audit all early returns in `handle_recv_cqe()` for this pattern.

### Delete direct recv-buffer `return_buffer(rc.buf_id)` call sites

After the refactor, no recv-path code should call `return_buffer(rc.buf_id)`
directly. All recv buffer recycling goes through `RecvSlices::recycle_all()` via
`buffer_slices_from_cqe` (normal path) or `discard_recv_bufs` (error path).

Audit `return_buffer` callers at `http_server.cxx:990+`. Non-recv buffer
recycling (send buffers, etc.) is out of scope.

In `phase3_dispatch()`, replace any leftover `return_buffer(rc.buf_id)` cleanup
with `discard_recv_bufs(rc)`. Phase 1 should consume all buffers, but this
becomes a defensive safety net: bundle CQEs missed by phase 1 would otherwise
silently leak multiple ring positions.

```cpp
// phase3_dispatch cleanup guard:
if (cqe_has_buffer(rc.flags))
    discard_recv_bufs(rc);
```

### Force-disable bundle until tests pass

```cpp
// In http_server init, after feature detection:
if (r.use_recv_bundle) {
    r.use_recv_bundle = false;
    log_warn("recv_bundle disabled: awaiting P0-01 test sign-off");
}
```

Remove this guard only after the tests below pass.

## Tests

### From original proposal

1. **Classic path uses consume()** — `buffer_slices_from_cqe(ring, res, flags, false)` for N CQEs; verify `debug_head_pos()` advances by 1 per CQE and slice ID matches expected recycle-order ID.

2. **Bundle: fits in one buffer** — `res = 100`, `buf_size = 8192`; verify single slice, `head_pos` +1.

3. **Bundle: exact N full buffers** — `res = 3 * buf_size`; verify 3 slices each `buf_size`, `head_pos` +3.

4. **Bundle: partial last buffer** — `res = 2 * buf_size + 500`; verify third slice is 500 bytes.

5. **Bundle: ring wraparound** — fill ring to within 2 slots of end; simulate bundle CQE spanning position `count_-1` and 0; verify correct IDs and byte lengths.

6. **Debug assert fires on desync** — manually advance `head_pos_` without consuming a CQE; call `buffer_slices_from_cqe`; verify assert fires on ID mismatch (debug builds only).

7. **`recycle_all()` restores pool** — after `recycle_all()` on a bundle slice, verify free count is restored and recycled IDs appear at expected `ring_order_` positions.

### Additional tests (added post-review)

8. **Non-sequential recycled IDs** — construct a ring state where consecutive positions contain IDs `{7, 2, 14}` (by recycling in that order). Simulate a 3-buffer bundle starting at ID 7. Verify `RecvSlices` yields `{7, 2, 14}`, not `{7, 8, 9}`. This is the exact failure mode of the original `(buf_id+1)%count` walk.

9. **Bundle discard path** — simulate invalid connection with `res = 2 * buf_size + 10`, `use_recv_bundle = true`. Call `discard_recv_bufs`. Verify `head_pos` advances by 3, not 1, and all 3 ring positions are recycled.

10. **Mixed CQE batch with interleaved non-recv completions** — feed a batch of CQEs: recv, send-complete, recv, timer. Verify `head_pos_` advances only for the two recv CQEs and in the order they are processed.

11. **No-buffer positive recv** — simulate `res > 0` with no `IORING_CQE_F_BUFFER`. Verify `buffer_slices_from_cqe` returns empty and no ring state mutates. The `assert(!cqe_has_buffer(flags))` in the function body does **not** fire here: `!cqe_has_buffer(flags)` is true, so the assertion holds. The assert is for the suspicious inverse: `res <= 0` **with** `IORING_CQE_F_BUFFER` set — that combination is the defect case to catch.

12. **Buffer-flag with negative res (debug assert)** — simulate `res = -ENOBUFS` with `IORING_CQE_F_BUFFER` set. Verify the debug assert fires (debug build only). This is the state the assert is guarding against.

13. **`dst.append()` throw safety** — if the project is not built `-fno-exceptions`, verify that a thrown exception from `dst.append()` still triggers `scope_exit` and calls `recycle_all()`. `head_pos_` must have advanced; `ring_order_` entries must be back in the recycle queue.

14. **Early return in `handle_recv_cqe()` discards buffer** — simulate a CQE with `IORING_CQE_F_BUFFER` and `ufd >= fd_table.size()`. Verify `discard_recv_bufs` is called, `head_pos_` advances, and no slot leaks.

## Implementation Order

1. Move `cqe_has_buffer()` and `cqe_buffer_id()` above `buffer_slices_from_cqe` in `socket_io.cxx`.
2. Change `RecvComp` to `{ fd, res, gen, flags }`.
3. Push `cqe->flags` in `handle_recv_cqe()` instead of pre-decoding `buf_id`.
4. Add `debug_head_pos()` to `BufferRing`.
5. Update `buffer_slices_from_cqe` signature to `(BufferRing&, int res, u32 flags, bool bundle)` with ID-match assert.
6. Add both `discard_recv_bufs` overloads.
7. Fix all early returns in `handle_recv_cqe()` to call `discard_recv_bufs(res, flg)`.
8. Replace `append_recv_buf_to()` with `RecvSlices` + `scope_exit`.
9. Delete all direct `return_buffer(rc.buf_id)` usage from recv flow.
10. Add `phase3_dispatch` safety net with `discard_recv_bufs(rc)`.
11. Force-disable runtime bundle with log warning.
12. Add unit tests 1–14 above.
13. Run existing HTTP benchmarks to confirm classic path has no regression.
14. Lift force-disable after all tests pass.

## Verification Targets

- `debug_head_pos()` advances by exactly `cnt` for every `buffer_slices_from_cqe` call.
- `ring_order_[head_pos % count]` equals the CQE's buffer ID before each `consume()` (asserted in debug).
- Server bundle path yields correct bytes for non-sequential-ID bundles (test 8).
- Bundle discard path consumes all ring positions, not just 1 (test 9).
- `res > 0` with no `IORING_CQE_F_BUFFER` returns empty, no ring mutation (test 11).
- `res < 0` with `IORING_CQE_F_BUFFER` triggers debug assert (test 12).
- Early return in `handle_recv_cqe()` with buffer flag calls `discard_recv_bufs` (test 14).
- No direct `return_buffer(rc.buf_id)` remains for recv buffers after refactor.
- Force-disable log appears when `recv_bundle=true` is configured.
- Classic HTTP benchmark shows no regression after refactor.

## Classification

| Item | Priority | Decision |
|---|---:|---|
| Fix server `(buf_id+1)%count` bundle walk | P0 | Fix: replace with `buffer_slices_from_cqe` |
| Route all recv recycling through consume-aware path | P0 | Fix: all recv paths through `buffer_slices_from_cqe` |
| `RecvComp`: store `flags`, drop `buf_id` | P0 | Amend: compact layout, no full `Cqe` |
| ID-match assert in `buffer_slices_from_cqe` | P0 | Add (plain `assert`, debug build only) |
| `debug_head_pos()` accessor | P0 | Add; not general public API |
| `discard_recv_bufs(int, u32)` + `(RecvComp&)` overloads | P0 | Add both; raw overload covers early returns |
| Early returns in `handle_recv_cqe()` | P0 | Fix: call `discard_recv_bufs(res, flg)` before return |
| `scope_exit` around append loop | P0 | Mandatory unless `-fno-exceptions` enforced in CI |
| `phase3_dispatch` safety net | P0 | Add `discard_recv_bufs(rc)` guard |
| Declaration order: helpers before `buffer_slices_from_cqe` | P0 | Prerequisite; move/forward-declare before editing |
| `consume_and_recycle(u16)` | — | Reject: unsafe for bundle error paths |
| Force-disable bundle until tests pass | P0 | Add with log warning |
| Unit tests 1–14 | P0 | Required before lifting force-disable |
| `incremental` buffer ring mode | P1 | Separate proposal (P1-05) |
