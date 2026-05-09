# DirectSlotPool Proposal (P0-04)

Status: proposal
Date: 2026-05-09

## Problem

`DirectFdTable` does sparse registration only. It has no concept of:
- which slots are free vs populated vs being closed
- kernel-allocated slots from `accept_direct` / `socket_direct_alloc`
- close failure → slot must be poisoned, not returned to the pool

Without a pool, direct-fd flows, socket accepts, and file flows all carry
independent slot ownership assumptions that drift incompatibly over time.

## Non-Goals

- Do not change `DirectFdTable` — it stays as the kernel registration wrapper.
- Do not fold slot ownership into `conflux.uring`.
- Do not build a lock-free pool; one thread owns one ring, pools are per-ring.
- Do not add slot adoption for IORING_OP_FIXED_FD_INSTALL results yet.

## Starting Point

`DirectFdTable` already handles:
- `io_uring_register_files_sparse`
- `io_uring_register_files_update(slot, fd)`
- `io_uring_unregister_files`

`DirectSlotPool` sits on top. It tracks state, allocates slots, and updates
`DirectFdTable` on population and close.

## Slot States

```
free → leased_empty → populated → closing → free
                                           ↘ poisoned (close failed)
```

- `free`: available for allocation
- `leased_empty`: allocated, not yet installed into the kernel table
- `populated`: fd installed via `register_files_update`
- `closing`: close SQE submitted, CQE not yet received
- `poisoned`: close failed; slot must never re-enter the free pool

Poisoned slots are permanent leaks. The log entry on poison is the diagnostic.

## Kernel-Allocated Slot Adoption

`accept_direct` and `socket_direct_alloc` have the kernel pick the slot.
The CQE result is the direct slot index. `DirectSlotPool` must adopt these
without going through `leased_empty`.

```
adopt_kernel_allocated(slot) → populated
```

The pool must validate the slot is in range and not already occupied.

## API

```cpp
class DirectSlotPool {
public:
    explicit DirectSlotPool(DirectFdTable& table) noexcept;

    [[nodiscard]] expected<u32, int> acquire() noexcept;
    [[nodiscard]] bool adopt_kernel_allocated(u32 slot) noexcept;
    void mark_populated(u32 slot, int fd) noexcept;
    void mark_closing(u32 slot) noexcept;
    void release_closed(u32 slot) noexcept;
    void poison(u32 slot, int close_res) noexcept;

    [[nodiscard]] u32 capacity() const noexcept;
    [[nodiscard]] u32 free_count() const noexcept;
    [[nodiscard]] u32 poisoned_count() const noexcept;
};
```

## Internal Storage

Flat `V<u8>` of `DirectSlotState` — one byte per slot, indexed by slot id.
Free list is a `V<u32>` maintained as a stack (push on release, pop on acquire).

No heap allocation in hot paths after construction.

## Wiring

| Site | Change |
|---|---|
| `http_server` init | Construct `DirectSlotPool` from `DirectFdTable` after registration |
| `handle_accept_cqe` | `adopt_kernel_allocated(res)` on direct accept |
| `queue_direct_accept_setup` | no change (recv arm is separate from slot state) |
| `handle_close_cqe` | `release_closed` on success, `poison` on failure |
| `uring.flow` direct-close | `mark_closing` before submit, `release_closed`/`poison` on CQE |

## Behavior Rules

- `acquire()` returns `unexpected(-ENOMEM)` when no free slots remain.
- `adopt_kernel_allocated` returns `false` if slot is out of range or not free.
- `mark_populated` calls `DirectFdTable::install(slot, fd)` internally.
- `poison` logs slot id and close_res; slot never returned to free list.
- `release_closed` resets slot to `free` and pushes to free list.
- All methods are single-threaded; one pool per ring.

## Verification Targets

- Direct accept CQE → `adopt_kernel_allocated` → slot in `populated` state.
- Close failure → slot in `poisoned` state → never reused.
- `free_count()` equals initial capacity minus poisoned minus in-use.
- Pool exhaustion returns `unexpected(-ENOMEM)`, not UB or crash.
- Pool integrates with existing `DirectFdTable` without changing its API.

## Implementation Order

1. Add `DirectSlotState` enum to `socket_io.cxx`.
2. Add `DirectSlotPool` class with `V<u8>` state + `V<u32>` free stack.
3. Wire into `http_server` init (replace bare `DirectFdTable` slot tracking).
4. Wire into `handle_accept_cqe` and `handle_close_cqe` in `http_server`.
5. Wire into `uring.flow` direct-close CQE handling.
6. Add resource-limit test: pool exhaustion, close failure → poison.
