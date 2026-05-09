# DirectSlotPool Proposal (P0-04)

Status: accepted, pre-implementation (amended post-review ×3)
Date: 2026-05-09

## Problem

`DirectFdTable` does sparse registration only. It has no concept of:
- which slots are free vs populated vs being closed
- kernel-allocated slots from `accept_direct` / `socket_direct_alloc`
- close failure → slot must be poisoned, not returned to the pool

Without a pool, direct-fd accept and close paths in `http_server` carry
incompatible slot ownership assumptions that drift over time and silently
reuse poisoned or occupied slots.

## Scope

**P0: `http_server` direct accept/close state mirror only.**

`acquire_explicit()`, `DirectSlotLease`, and `uring.flow` integration are P1.
Do not wire explicit leasing while multishot direct accept is armed — kernel
auto-allocation and userspace leasing share the same table and collide unless
explicitly partitioned by range.

## Encapsulation Constraints

`DirectSlotPool` is **internal to `http_server`**. Direct slot ids, pool
state, and `DirectSlotError` must not appear in any public-facing `http_server`
API or user handler signature. If direct slot metadata is ever needed outside
internals, surface it only via diagnostics (counts, logged strings), never as
raw ids or enum values. This prevents the Beast/uWebSockets trap where
high-performance internals become the user's mental model.

`DirectSlotError` is internal. If `DirectSlotPool` ever surfaces through a
public API boundary, the error type must be either `std::expected`-compatible
or replaced with a public-vocabulary equivalent. Do not expose it directly.

## Non-Goals

- Do not change `DirectFdTable`.
- Do not fold slot ownership into `conflux.uring`.
- Do not build a lock-free pool; one thread owns one ring, pools are per-ring.
- P0 does not model `openat_direct`, `pipe_direct`, `msg_ring_fd_alloc`, or
  `fixed_fd_install` cleanup paths.
- P0 does not integrate with `uring.flow` close/result paths.

## Starting Point

`DirectFdTable` already handles:
- `io_uring_register_files_sparse`
- `io_uring_register_files_update(slot, fd)`
- `io_uring_unregister_files`

`DirectSlotPool` sits on top. Tracks state, mirrors kernel-allocated slots,
coordinates close semantics.

## Slot States (P0)

```
adopt_kernel_allocated() ──→ populated → closing → free
                                                  ↘ poisoned (close failed)
```

- `free`: in free list, available
- `populated`: kernel-installed (accept/socket_alloc) or userspace-installed fd
- `closing`: close SQE submitted, CQE pending
- `poisoned`: close failed; never re-enters free list

`leased_empty` does not exist in P0. Unsafe while kernel auto-allocation is armed.

Poisoned slots are permanent leaks. The log entry on `poison()` is the diagnostic.

## Kernel-Allocated Slot Adoption

`accept_direct` / `socket_direct_alloc` have the kernel pick the slot. CQE result
is the direct slot index. Pool adopts without going through a userspace lease:

```
adopt_kernel_allocated(slot) → populated    [O(1) via free_pos swap-remove]
```

Validates slot is in range and currently free.

## Error Type

```cpp
enum class DirectSlotError : u8 {
    not_registered,
    exhausted,      // capacity full
    out_of_range,
    bad_state,
    install_failed,
};
```

**Implementation note (amended):** The original spec used `int` with `-errno` values
(`-ENODEV`, `-ENOSPC`, etc.) for potential errno-channel compatibility. This was revised
to `u8` with sequential ordinals because: (a) `clang-tidy performance-enum-size` flags
the `int` base as oversized for a 5-value enum; (b) the type is strictly internal and
never surfaces at any errno boundary; and (c) the `std::expected` return makes
errno-channel mapping unnecessary. Any future use at an errno boundary must translate
explicitly.

## API

```cpp
class DirectSlotPool {
public:
    explicit DirectSlotPool(DirectFdTable& table); // may throw on V alloc

    // called at init for listening socket slot
    expected<void, DirectSlotError> install_os_fd(u32 slot, int fd) noexcept;

    // called from handle_accept_cqe / handle_socket_alloc_cqe
    expected<void, DirectSlotError> adopt_kernel_allocated(u32 slot) noexcept;

    // called only after SQE construction succeeds — see ordering rule below
    expected<void, DirectSlotError> mark_closing(u32 slot) noexcept;

    // called from close CQE handler on success; valid only from closing
    expected<void, DirectSlotError> release_closed(u32 slot) noexcept;

    // called from close CQE handler on failure; tolerates ugly states but logs them
    void poison(u32 slot, int close_res) noexcept;

    [[nodiscard]] u32 capacity()       const noexcept;
    [[nodiscard]] u32 free_count()     const noexcept;
    [[nodiscard]] u32 poisoned_count() const noexcept;
};
```

`acquire_explicit()` and `DirectSlotLease` are P1 additions. Not present in P0.

## Internal Storage

```cpp
V<u8>  state;      // DirectSlotState per slot, indexed by slot id
V<u32> free_stack; // stack of free slot ids
V<u32> free_pos;   // slot → index in free_stack; UINT32_MAX if not free
```

- `release_closed(slot)`: push to `free_stack`, record position in `free_pos`
- `adopt_kernel_allocated(slot)`: swap-remove from `free_stack` in O(1) using `free_pos[slot]`
- `install_os_fd(slot, fd)`: swap-remove slot from free list, mark `populated`

Invariant after `install_os_fd(listen_fd, listen_fd)`:

```text
state[listen_fd]    == populated
free_pos[listen_fd] == UINT32_MAX
listen_fd not in free_stack
```

No heap allocation in hot paths after construction.

## State Transition Rules

### `mark_closing(slot)`
- Valid only from `populated`.
- Returns `bad_state` from any other state.
- Must be called **only after SQE construction succeeds**:

```cpp
bool submit_direct_slot_close(..., u32 slot, u64 close_ud) {
    auto* sqe = ring_.get_sqe();
    if (!sqe)
        return false;
    prep_close_direct(sqe, slot);
    set_user_data(sqe, close_ud);
    auto ok = direct_slots_->mark_closing(slot);
    if (!ok)
        return false;   // do not submit; state inconsistent
    return true;
}
```

For shutdown → close_direct chains, acquire both SQEs and prep the close SQE
before calling `mark_closing`.

**Implementation note (amended):** `queue_close` calls `mark_closing` after
`submit_shutdown_close` (which prepares **and** submits both SQEs), not before.
This is safe because `fd_table[ufd].closing` is checked at `queue_close` entry
and prevents double-entry. If `mark_closing` returns `bad_state` after submit,
the slot is already in an inconsistent upstream state; the log entry is the
diagnostic. The pattern above remains the canonical form for new submission
helpers; `queue_close` is a pre-existing multi-SQE path where splitting is
not practical.

### `release_closed(slot)`
- Valid only from `closing`.
- Returns `bad_state` from `free`, `populated`, or `poisoned` (prevents duplicate free entries).

### `poison(slot, res)`
- Tolerates any in-range non-free state, but logs previous state:
  `slot=N close_res=-EBADF previous_state=closing/populated/...`
- If called on `free`: log loudly as state corruption; do not double-push.
- If called on `poisoned` (re-poison from duplicate CQE): log; do **not**
  increment `poisoned_count_` again. The counter must reflect distinct poisoned
  slots, not CQE count.
- Asserts `free_pos_[slot] == sentinel` — callers must ensure slot is not in
  the free list. This holds for `populated`, `closing`, and `poisoned` states.

## Op::DirectSlotClose

Add a dedicated op for direct-slot close SQEs. Do **not** replace `Op::Nop` globally —
it remains valid for shutdown CQEs, quickack setup, and other ignored completions.

```cpp
enum class Op : u8 {
    ...
    DirectSlotClose,
    Nop
};
```

```cpp
case Op::DirectSlotClose:
    handle_direct_slot_close(fd, res);
    break;
```

Replace only direct-slot close SQE user_data. All of these must route through
`Op::DirectSlotClose`:

| Close site | Action |
|---|---|
| Normal connection close | `Op::Close` (see note) |
| WS fixed-fd-install slot cleanup | `Op::DirectSlotClose` |
| Handoff failure cleanup | `Op::DirectSlotClose` |
| Standalone direct-slot cleanup | `Op::DirectSlotClose` |

**Implementation note (amended):** Normal connection close retains `Op::Close`
and dispatches to `handle_conn_close(fd, res, gen)`. This is necessary because
`conn_erase(fd, gen)` requires the generation counter to prevent stale CQE
collisions; `Op::DirectSlotClose` / `handle_direct_slot_close` does not carry
`gen`. `handle_conn_close` calls `release_closed` / `poison` on the pool before
calling `conn_erase`, so pool lifecycle is fully managed. The key invariant is
that every close CQE for a direct slot reaches pool state update — not which
op tag carries it.

## Adopt Failure Handling

Do **not** flip `fixed_files = false` live while direct-slot connections are active.
`fixed_files` is used by send/recv/close paths; flipping it reinterprets direct
slot IDs as OS fds for existing connections.

On `adopt_kernel_allocated(res)` failure:

```text
1. Log loudly (slot, res, current state).
2. Cancel multishot accept.
3. Set fixed_accept_enabled = false — stop accepting new fixed-file conns.
4. Best-effort close_direct(res) if res is in range and plausibly populated.
5. Keep fixed_files = true for existing active connections.
6. Optionally initiate graceful server shutdown.
```

```cpp
bool fixed_accept_enabled = true;  // can flip false on adopt failure
bool fixed_files = true;           // do NOT flip live while direct conns exist
```

## Wiring (P0)

| Site | Change |
|---|---|
| `http_server` init | Construct `DirectSlotPool` only if `DirectFdTable` registered; `install_os_fd(listen_fd, listen_fd)` |
| `handle_accept_cqe` | `adopt_kernel_allocated(res)` before `conn_for(res)`; on failure: cancel accept, stop fixed accept |
| Direct-slot close SQEs | Replace `Op::Nop` with `Op::DirectSlotClose` |
| `handle_direct_slot_close` CQE | `release_closed` on success, `poison(slot, res)` on failure |

`uring.flow` close/result integration is **P1**.

## Performance

For `MAX_FILES = 65536`:

```
state:      65,536 bytes
free_stack: 262,144 bytes
free_pos:   262,144 bytes
total: ~576 KiB
```

Per-CQE cost: a few indexed loads/stores. Correctness gain >> cost.

Flat `V<u8>` / `V<u32>` arrays follow the data-layout-first guidance: no
heap allocation on hot paths, cache-friendly sequential access, zero
synchronization (one thread owns one ring).

`alignas(64)` on `DirectSlotPool` is optional — only apply if the object
is stored adjacent to other hot ring fields and false-sharing is measured.

## Verification Targets

- Direct accept CQE → `adopt_kernel_allocated` → slot in `populated` state.
- Duplicate `adopt_kernel_allocated` on same slot → `bad_state`.
- `install_os_fd(listen_fd, ...)` → listener slot excluded from free list; draining remaining slots never returns slot 0.
- Close failure → slot in `poisoned` state → never reused.
- `release_closed` from non-`closing` state → `bad_state`.
- `mark_closing` from `free_slot`, `closing`, `poisoned` → `bad_state`; out-of-range → `out_of_range`.
- `poison` from `populated` (no prior `mark_closing`) → `poisoned` state.
- Re-`poison` on already-poisoned slot → `poisoned_count` unchanged.
- `free_count()` equals initial capacity minus poisoned minus in-use.
- Pool exhaustion returns `DirectSlotError::exhausted`, not UB or crash.
- Pool integrates with existing `DirectFdTable` without changing its API.
- **Integration (not covered by pool unit tests):** adopt-failure → `fixed_accept_enabled=false`, `fixed_files` unchanged, existing connections unaffected.

## Implementation Order (P0)

```text
[ ] Add DirectSlotState + DirectSlotError.
[ ] Add DirectSlotPool with state/free_stack/free_pos.
[ ] Implement O(1) free-stack remove (swap-remove) for install_os_fd and adopt.
[ ] Add invariant asserts for free_pos/state consistency.
[ ] Construct pool only when DirectFdTable registered.
[ ] install_os_fd(listen_fd, listen_fd); if fail, keep fixed_files=false, skip pool.
[ ] handle_accept_cqe: adopt_kernel_allocated(res) before conn_for(res).
[ ] On adopt failure: cancel accept, set fixed_accept_enabled=false; do not flip fixed_files.
[ ] Add Op::DirectSlotClose.
[ ] Replace only direct-slot close SQE user_data with Op::DirectSlotClose.
[ ] Route normal connection direct close through pool (mark_closing → CQE handler).
[ ] Route WS fixed-fd-install cleanup close through pool.
[ ] Route handoff-failure direct close through pool.
[ ] On close CQE success: release_closed(slot).
[ ] On close CQE failure: poison(slot, res).
[ ] Add tests: adopt, duplicate adopt, close success, close failure poison,
    duplicate release, listener slot exclusion, adopt-failure no-flip-fixed_files.
[ ] Confirm DirectSlotPool not exposed through any public http_server API.
[ ] Adjacent P0/P0.5: after direct accept adoption, apply TCP_NODELAY / TCP_QUICKACK
    to accepted direct sockets via a direct-fd-safe mechanism (io_uring socket-option op
    or fixed-fd-install handoff). If not yet feasible for direct slots, document the gap.
```

## P1

```text
[ ] DirectSlotLease + acquire_explicit() — only safe when kernel auto-allocation
    not armed, or kernel-auto / explicit ranges partitioned.
[ ] leased_empty state.
[ ] complete_explicit_direct_open(slot, res).
[ ] uring.flow close/result integration — requires FlowResult to carry DirectSlot
    identity, or per-flow callback capturing slot + ownership source.
[ ] open_direct / pipe_direct / msg_ring_fd_alloc / fixed_fd_install generalized ownership.
```
