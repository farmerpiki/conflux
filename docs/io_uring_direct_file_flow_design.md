# io_uring Direct File Flow API Design (v2)

## Goal

Provide a high-level C++ API over `io_uring` that makes direct/fixed-file-slot usage natural while preserving correct resource lifetime semantics.

The API must hide repetitive fixed-file details from normal user code:

- `DirectSlot{k}`
- fixed-file descriptor wrappers
- `IOSQE_FIXED_FILE`
- explicit cleanup bookkeeping
- per-operation CQE accounting
- soft/hard link flag selection
- chained-vs-standalone cleanup decisions

Design principles (in priority order):

1. **Easy to do the right thing; hard to do the wrong thing.** The API must auto-pick the optimal correct emission mode whenever the user-supplied chain shape allows it, and must reject constructions that would cause silent resource leaks.
2. Zero dynamic allocation in the submit/CQE hot path.
3. Zero-copy path and buffer handling by default.
4. Fixed-capacity flow state.
5. Slab-indexed state lookup.
6. No internal locking in the flow layer.
7. No hidden fallback to `IOSQE_IO_DRAIN`.

The API must not pretend that `IOSQE_IO_LINK` supports block-structured `finally` semantics. Cleanup that cannot be safely placed inside the linked chain must be implemented by userspace flow tracking.

---

## Integration Contract

The flow layer does **not** own global ring policy.

The surrounding framework is responsible for:

1. Ring sizing.
2. SQ backpressure and SQE acquisition. The framework must expose a non-blocking try-acquire that may return null when the SQ is full.
3. **Reserving SQ slots for an entire initial linked chain before flow emission begins.** `IOSQE_IO_LINK` does not span submission boundaries; a partial chain spread across two `io_uring_enter` calls is silently broken at the gap. This applies to mode A chains (open + body) and mode B chains (open + body + close); see *Cleanup Mode Selection*.
4. SQE emission serialization for a given ring.
5. Threading and locking.
6. Direct-slot leasing/allocation.
7. Ring submit batching.
8. **Deferred retry hook for cleanup SQEs that cannot acquire an SQ slot at completion time** (mode A only); see *Cleanup Backpressure*.
9. Routing all managed CQEs into the flow runtime.
10. Project-wide `O_CLOEXEC` default policy on real-fd opens. Not relevant to direct-slot opens; see *Kernel Operation Mapping*.
11. **CQ sizing and overflow handling.** The flow runtime relies on observing every managed CQE; a permanently-lost CQE leaks the slab cell and (for mode A) the direct-slot lease. The framework must size the CQ ring large enough that managed CQEs are not dropped under expected load, and must drain the kernel's overflow list (or treat sustained overflow as fatal) before declaring a flow stuck. The flow layer does not implement timeouts on missing CQEs.
12. **Committing runtime-emitted SQEs.** When the flow runtime emits a cleanup SQE outside a `flow.submit()` call (mode A standalone close, including the deferred-retry path via `resume_deferred_close`), the framework must include that SQE in the next submit batch before the ring sleeps or the runtime stalls. Either every `try_acquire_sqe()` call by the flow runtime is paired with an implicit commit on the framework's next submit, or the framework exposes a `commit_runtime_sqe()` hook that the runtime calls after `submit_close_direct`. The flow layer assumes the former unless specified otherwise.

The flow layer assumes:

- SQE emission for a given ring is serialized externally.
- A `DirectSlot` passed to a flow is exclusively leased to that flow until cleanup completes.
- The direct descriptor table is already registered and large enough for the requested slot.
- The CQE handler receives every CQE belonging to managed flow operations.
- Flow-state slab entries are not reused without changing their generation.
- Submit/CQE hot paths must avoid heap allocation.
- The framework provides a deferred-cleanup hook the runtime can call when SQ space frees.

This document intentionally does not specify ring capacity management, locking, scheduler wakeups, or cross-thread ownership rules.

---

## Terminology

Use **flow** for structured userspace-managed operation groups.

Use **chain** or **linked chain** for raw `io_uring` linked-SQE semantics.

Recommended split:

```cpp
ring.chain(); // low-level, exact io_uring link semantics
ring.flow();  // structured CQE-tracked flow semantics
```

A raw linked chain must not expose names that imply `finally` semantics unless the implementation really provides structured cleanup through userspace CQE tracking.

---

## Kernel Operation Mapping

### Minimum kernel

The flow API targets Linux **5.15** or later. Required features:

- `io_uring_prep_openat_direct` / `io_uring_prep_close_direct` (5.15+).
- `IORING_FEAT_SUBMIT_STABLE` (5.5+) for the relaxed path-lifetime contract; without it the path must remain valid until the open CQE is observed (see *Lifetime Requirements*).

The framework must refuse ring initialization on older kernels. Out-of-scope SQE flags (`IOSQE_ASYNC`, `IOSQE_BUFFER_SELECT`) must not appear on managed-flow SQEs in v1; the runtime must not propagate framework-defaulted flags into chain SQEs without an explicit per-flow opt-in (none exists in v1).

### Direct open / body / close

`open_direct(slot, dfd, path, open_flags, mode)` is a library-level operation implemented with:

```cpp
io_uring_prep_openat_direct(...)
```

or:

```cpp
io_uring_prep_openat2_direct(...)
```

For explicit slots, the ring must already have a registered direct descriptor table large enough to contain `slot`.

If a specified direct slot already contains a file, direct-open semantics replace the slot contents and close the old file. Therefore, accidental slot reuse is a correctness bug. The flow API assumes the surrounding framework provides exclusive slot leasing.

The directory file descriptor `dfd` is a normal file descriptor, not a fixed-file descriptor.

Body operations issued through the flow use the direct slot as the SQE file descriptor and set `IOSQE_FIXED_FILE`.

`close_direct(slot)` is implemented with:

```cpp
io_uring_prep_close_direct(sqe, slot.value);
```

The liburing signature is `void io_uring_prep_close_direct(io_uring_sqe*, unsigned file_index)`. The slot index is passed as `file_index`; it is **not** written to the SQE `fd` field, and there is no `unused` third argument.

Do **not** set `IOSQE_FIXED_FILE` on this SQE — the kernel completes such SQEs with `-EBADF`. The op operates on the direct descriptor table, not on a fixed file in that table.

`close_direct(slot)` is not a normal `close(fd)` on a regular file descriptor. It clears/closes the direct descriptor slot.

### `O_CLOEXEC`

Project-wide policy is to set `O_CLOEXEC` on all real-fd opens. This is required because the library hosts a `process` module that may fork/exec; non-`O_CLOEXEC` fds would leak into spawned children.

For `openat_direct`, the bit is functionally harmless: direct slots live in the io_uring direct descriptor table, not in the process file-descriptor table, and are not inherited across `execve`. Users may pass `O_CLOEXEC` in `open_flags` for consistency with the rest of the codebase.

The flow layer never mutates user-supplied `open_flags`.

---

## Link Semantics Reference

Knowledge of kernel cancel-cascade behavior is required to understand the cleanup mode selection rules below.

`IOSQE_IO_LINK` (soft link) on SQE i:

- SQE i+1 starts only after SQE i completes.
- If SQE i fails (including short read/write), SQE i+1 is cancelled with `-ECANCELED`. The kernel then walks the entire downstream link list and cancels every linked successor unconditionally.

`IOSQE_IO_HARDLINK` (hard link) on SQE i:

- SQE i+1 starts after SQE i completes, regardless of SQE i's result.
- Hard link is **only** resilient to the *executed* result of SQE i. If SQE i never executed (cancelled by an upstream soft-link failure), the cancellation cascade still kills SQE i+1 and everything beyond.

Implication used throughout this document:

> Once a soft-link failure starts a cancellation cascade, the cascade kills every downstream SQE in the same chain, **including hard-linked successors**.

Short read/write counts as failure for the purpose of `IOSQE_IO_LINK`. A short read with soft link to the next SQE cancels that next SQE.

There is no policy switch to disable cascade. The only escape is to use `IOSQE_IO_HARDLINK` *upstream of the point of failure*, so the failure happens at an executed-and-failed boundary that hard link can cross.

---

## API Convention: link-mode selectors

Each appended body operation chooses how it is linked **from its predecessor**:

```cpp
f.then_read (...)   // predecessor SQE gets IOSQE_IO_LINK     (soft)
f.then_write(...)
f.hard_read (...)   // predecessor SQE gets IOSQE_IO_HARDLINK (hard)
f.hard_write(...)
```

The variant chosen for op[i] sets the SQE flag of op[i-1]. Read left-to-right:

```cpp
f.then_read(...)         // open is soft-linked to this read
 .hard_write(...);       // read is hard-linked to this write
```

emits:

```text
open  IOSQE_IO_LINK
read  IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK
write IOSQE_FIXED_FILE
```

### First-body restriction

The first body op must be `then_*`. `hard_*` as the first body is rejected by the builder because it would set `IOSQE_IO_HARDLINK` on the open: an open-fail would no longer cancel the body, and the body would run against an unleased slot. This is a footgun; the builder marks the flow invalid (`last_error() == -EINVAL`).

There is no `hard_open` or equivalent. Open is always soft-linked to its successor (or terminal, if there is no successor and no auto-close).

### No `read()` / `write()` aliases

There are exactly four body verbs: `then_read`, `then_write`, `hard_read`, `hard_write`. There is no shorter `read` or `write` alias.

---

## Desired User Syntax

### Scoped Form (default)

Default-safe shape — `then_read` + `then_write` describes "read header; if read succeeds, write payload":

```cpp
auto flow = ring.flow();

flow.with_direct_file(k, dfd, path, O_RDWR | O_CLOEXEC, 0, [&](auto f) {
    f.then_read (header,  header_len,  0)
     .then_write(payload, payload_len, header_len);
});

flow.submit();
```

This means:

1. Open `path` into direct slot `k`.
2. If open succeeds, read header.
3. If read succeeds (no failure, no short read), write payload.
4. After all CQEs for the chain have been observed, close slot `k` if and only if open succeeded.
5. No `IOSQE_IO_DRAIN`.
6. No allocation in the submit/CQE hot path.

`close_if_opened()` is implicit at scope end. The lambda is a synchronous builder callback; it only records operations.

The name reads as "close iff open succeeded": the close runs whenever the open CQE returned `>= 0`, regardless of body-op outcomes. It does **not** mean "close iff the whole flow succeeded." Body failures do not suppress the close — that is the entire point of mode B's hard-tail rule.

The all-`then_*` shape routes to mode A (standalone close): the chain itself fits one `io_uring_enter`, and a follow-up close is submitted post-chain by the runtime.

### Mode B optimization

If the write is logically valid even when the read fails or returns short, switch the dependent boundary to hard. The chain becomes mode B (single `io_uring_enter`, in-chain close):

```cpp
flow.with_direct_file(k, dfd, path, O_RDWR | O_CLOEXEC, 0, [&](auto f) {
    f.then_read (probe,   probe_len, 0)
     .hard_write(payload, payload_len, payload_offset);
});
```

`hard_write` here means "write runs even if read failed or was short." This is correct only when the write does not depend on read's data or completeness. Use deliberately, not as a default.

### Explicit Form

Equivalent low-level syntax:

```cpp
auto flow = ring.flow();

auto f = flow.open_direct(k, dfd, path, O_RDWR | O_CLOEXEC);

f.then_read (header,  header_len,  0);
f.then_write(payload, payload_len, header_len);
f.close_if_opened();

flow.submit();
```

The scoped form compiles internally to this same model.

`close_if_opened()` on a flow that is already invalid (`valid() == false`) is a no-op, consistent with the rest of the no-op-on-invalid-flow rule.

### Lambda handle lifetime

The `f` value passed into the scoped lambda is a cheaply-copyable handle that refers to builder-owned storage in `flow`. It must not be held across coroutine suspension or yield boundaries; use only synchronously inside the lambda body.

---

## Required Semantics

Conceptually, the flow emits an initial linked chain. Cleanup placement depends on chain shape (see *Cleanup Mode Selection*).

```text
open_direct(slot k)  IOSQE_IO_LINK
read(slot k)         IOSQE_FIXED_FILE | (IO_LINK or IO_HARDLINK)
write(slot k)        IOSQE_FIXED_FILE | (IO_LINK or IO_HARDLINK or terminal)
[close_direct(k)]    terminal, optional in-chain
```

The flow result must preserve:

- open result;
- each body operation result;
- close result, if cleanup was submitted.

Cleanup failure must be reported but must not erase the original body failure.

---

## Cleanup Mode Selection

The flow runtime emits the close in one of two modes, picked automatically based on the user-built chain shape. The user does not select the mode directly.

### Mode A — standalone cleanup (post-CQE)

`close_direct` is **not** part of the initial linked chain. After every CQE of the initial chain has been observed, the runtime submits a standalone `close_direct` SQE.

- A second submission phase is required for the cleanup SQE; it may be batched with other framework SQEs and is not necessarily a dedicated `io_uring_enter` per flow.
- Close not counted in `expected_cqes`.
- Close-CQE handled separately; `finish_flow` waits for both `seen_cqes == expected_cqes` and `close_seen` (when close was submitted).
- Subject to SQ backpressure at close-submit time; see *Cleanup Backpressure*.
- Always safe regardless of chain shape.

### Mode B — chained cleanup (in-chain close)

`close_direct` is appended as the terminal SQE of the linked chain itself.

- One `io_uring_enter` for the entire flow.
- Close counted in `expected_cqes`.
- Close-CQE handled like body CQEs; `finish_flow` runs when `seen_cqes == expected_cqes`.
- No deferred-submit logic.
- Eligibility constrained (see below).

### Auto-selection rule

`mode_b_eligible(b)` is a pure shape predicate over user-recorded ops. It is only consulted when `close_requested == true`; a flow with no `close_if_opened()` always emits the chain as-is with no synthesized close.

At submit time the runtime evaluates:

```text
mode_b_eligible iff:
    op_count >= 1                              (open present)
    AND (op_count == 1                         (just open + auto-close)
         OR (ops[1].variant == then_           (open→body[1] soft so open-fail cascades)
             AND for i in 2..op_count-1:       (every later body op)
                 ops[i].variant == hard_))     (each body→next boundary hard)
```

A flow uses **mode B** iff `close_requested && mode_b_eligible(b)`. Otherwise it uses **mode A** (close still runs, but standalone post-CQE).

Rationale, by case:

| chain shape | mode | reasoning |
|---|---|---|
| open alone (auto-close) | B | open→close soft (auto). open-fail cancels close. open-ok runs close. |
| open + then_read | B | open→read soft; read→close hard (auto). open-fail cancels everything; read-fail still runs close. |
| open + then_read + hard_write | B | open→read soft; read→write hard; write→close hard (auto). read-fail still reaches write and close. |
| open + then_read + hard_write + hard_X | B | tail stays hard. |
| open + then_read + then_write | A | read→write soft. read-fail cascades through write to close → leak. Falls back to mode A; close standalone. |
| open + hard_read + ... | invalid | open→read hard would mean read runs after open-fail against an unpopulated slot. Rejected. |
| open + then_read + hard_write + then_X | A | trailing soft re-introduces cascade reaching close. |

Why mode B requires the *first* link (open→first-body) to be soft and *every other* link to be hard:

- Open-fail must cancel the close. The slot was never populated in the kernel direct-table; running `close_direct` on an empty slot is wasted work and produces a confusing `-EBADF` (the kernel rejects close-on-empty-slot without side effect — verified against liburing test/open-close.c). Skipping the close is the cleanest observable behavior.
- A soft link from open to the first body causes open-fail to cascade to the entire chain, including the close. ✓
- Body-op failures must NOT cancel the close (open succeeded; slot is populated and must be closed).
- Hard links between body ops and from the last body to the close prevent cascade through the body section. ✓

### Eligibility violation handling

If the user constructs a chain where the first body op is `hard_*`, the builder marks the flow invalid and stops appending. The motivation is "easy to do the right thing": a hard-linked first body would run after an open failure against a kernel direct-table entry that was never populated, completing with `-EBADF`. The kernel handles this cleanly, but it is wasted work and produces confusing error patterns; rejecting at build time eliminates the footgun.

`flow.valid()` returns false; `flow.last_error()` returns `-EINVAL`; subsequent appends (including `close_if_opened()`) are no-ops; `flow.submit()` rejects this flow without emitting partial SQEs. Other valid flows in the same batch still submit.

There is no "force mode A" or "force mode B" knob. The auto-rule produces the correct mode for every legal construction.

---

## Flow Object Responsibilities

`ring.flow()` creates a builder/controller responsible for:

1. Allocating or receiving a flow-state slab index.
2. Recording operations into fixed-capacity storage.
3. Emitting SQEs.
4. Attaching encoded `user_data` tags to each SQE.
5. Tracking expected CQEs per flow.
6. Recording the direct-open result.
7. Recording every body result.
8. Detecting when the chain has completed.
9. Submitting cleanup SQEs (mode A) or counting in-chain cleanup CQEs (mode B).
10. Recording cleanup results.
11. Finishing user-visible flow results.

The flow layer is allowed to submit more SQEs during CQE processing in mode A, but the surrounding framework owns SQ availability and emission serialization.

---

## Static Capacity Rules

The v1 flow implementation uses fixed-capacity storage.

```cpp
static constexpr uint8_t  max_initial_ops = 8;          // open + body ops the user can append
static constexpr uint8_t  max_chain_cqes  = max_initial_ops + 1; // +1 for mode-B in-chain close
static constexpr uint32_t kMaxFlows       = 4096;       // slab capacity (flow_index range)
```

`kMaxFlows` bounds the flow-state slab. The value above is a placeholder; the runtime owns the constant, but the chosen value is policy-adjacent — pick based on expected concurrent in-flight flow count plus headroom for deferred-close registrations. `flow_index` in `user_data` reserves 24 bits, so `kMaxFlows` must be `≤ 2^24`.

A flow contains:

- one direct open operation (always op[0]);
- zero or more dependent body operations (op[1..op_count-1]);
- at most one cleanup operation (in-chain in mode B, counted as op[op_count]; standalone in mode A, not stored in `ops`).

The cleanup op is **synthesized at submit time**; it does not consume builder-side `ops` storage. The result for the close lives in a separate `close_res` field, not in the `results` array.

Storage uses `std::array` only. No third-party containers. No `std::vector`, no allocators in the hot path. No new dependencies.

```cpp
std::array<pending_op, max_initial_ops> ops;       // user-recorded ops (open + body)
std::array<op_result,  max_initial_ops> results;   // body/open results only; close_res is separate
uint8_t op_count = 0;
```

If the user tries to append more than `max_initial_ops` initial-chain operations, the builder marks the flow invalid (`last_error() == -ENOBUFS`). The synthesized close in mode B never overflows because `max_chain_cqes == max_initial_ops + 1` is reserved at the runtime boundary, not the builder boundary.

The submit/CQE hot path must not allocate.

---

## Builder Error Surface

The builder must report errors without allocation and without throwing. Project compiles with `-fno-exceptions` are valid targets.

```cpp
class direct_file_flow {
public:
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int  last_error() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};
```

Semantics:

- `valid()` is true on a fresh flow and stays true as long as every appended op is legal.
- `last_error()` returns 0 on a valid flow, or a negative errno-style code (`-EINVAL`, `-ENOBUFS`) on an invalid flow.
- All mutating calls — `then_read`, `then_write`, `hard_read`, `hard_write`, and `close_if_opened` — are no-ops on an invalid flow. The first violating call sets `last_error()`; subsequent calls do not overwrite it.
- `flow.submit()` skips invalid flows and continues with valid ones in the same batch.

Builder error sources in v1:

- `-EINVAL`: first body op is `hard_*`.
- `-ENOBUFS`: appending past `max_initial_ops` (open + body counted together).
- `-EOVERFLOW`: a `len` argument exceeds `INT32_MAX`. Public read/write signatures take `size_t len`; internal `pending_read::len` / `pending_write::len` are `uint32_t`, but `cqe->res` is `int32_t` and a successful completion must be representable as a non-negative `int32_t`. The builder rejects `len > INT32_MAX` at append time rather than silently narrowing or producing an unrepresentable success value.

Rejection diagnostics are exposed via the framework, not via the flow object. The framework's submit-side hook receives one rejection record per skipped flow:

```cpp
struct flow_rejection {
    uint32_t flow_local_index;   // index within the flow_builder batch
    int      err;                // see canonical error list below
};
```

Canonical rejection errors:

- **Builder-side** (set during `then_*`/`hard_*`/`close_if_opened`/`open_direct`):
  - `-EINVAL`: first body op is `hard_*`.
  - `-ENOBUFS`: appending past `max_initial_ops` (open + body counted together).
  - `-EOVERFLOW`: a `len` argument exceeds `INT32_MAX`.
- **Submit/runtime admission**:
  - `-EAGAIN`: framework could not reserve contiguous SQ slots for the chain.
  - `-ENOSPC`: flow-state slab full (no free `flow_index`).
  - `-EOPNOTSUPP`: lifetime contract cannot be satisfied (e.g., `IORING_FEAT_SUBMIT_STABLE` absent and no fallback copy available).

V1 only requires that the framework can read these per-batch. A `flow_builder::rejected_flows() -> std::span<const flow_rejection>` accessor is acceptable; so is a callback. The shape of the accessor is framework-defined; the contract is that no flow is silently dropped without a recorded reason. The returned span (if accessor form) is valid until the next `flow.submit()` call or the next builder mutation.

### Slot-lease ownership for rejected flows

For every entry in `rejected_flows()`, the framework retains ownership of the leased direct slot it passed to `open_direct(...)` and must return it to the slot allocator. The flow layer never touches the lease for a rejected flow — no slab cell was allocated, no SQE was emitted, and no close will be issued. This rule applies uniformly to builder-side and submit-side rejections.

---

## Internal State Model

```cpp
// (See Static Capacity Rules)
// static constexpr uint8_t max_initial_ops = 8;

enum class flow_op_kind : uint8_t {
    open_direct,
    read,
    write,
    close_direct,
};

enum class link_variant : uint8_t {
    then_,    // predecessor SQE will get IOSQE_IO_LINK
    hard_,    // predecessor SQE will get IOSQE_IO_HARDLINK
};

struct op_result {
    int32_t       res = 0;
    uint32_t      requested = 0;
    flow_op_kind  kind = flow_op_kind::open_direct;

    [[nodiscard]] bool ok() const noexcept { return res >= 0; }

    [[nodiscard]] bool is_io() const noexcept {
        return kind == flow_op_kind::read || kind == flow_op_kind::write;
    }

    [[nodiscard]] bool short_io() const noexcept {
        return is_io() && res >= 0 && uint32_t(res) < requested;
    }

    [[nodiscard]] bool full_io() const noexcept {
        return is_io() && res >= 0 && uint32_t(res) == requested;
    }
};

struct direct_file_flow_state {
    uint32_t flow_index;
    uint32_t generation;          // 24-bit usable; gen 0 reserved (see Generation Discipline)

    DirectSlot slot;

    uint8_t initial_op_count = 0; // open + body count (== b.op_count); bounds for results[]
    uint8_t expected_cqes = 0;    // mode A: open+body count; mode B: open+body+close count
    uint8_t seen_cqes = 0;

    bool open_seen = false;
    bool open_ok = false;
    int32_t open_res = 0;

    bool close_requested = false; // user called close_if_opened on a valid flow
    bool close_in_chain = false;  // close SQE was actually emitted as part of the chain
                                  // (mode B). Implies counted in expected_cqes.
    bool close_submitted = false; // mode A: standalone close SQE has been emitted
    bool close_pending = false;   // mode A: deferred due to SQ-full
    bool close_seen = false;      // a close CQE has been observed (mode A or mode B)
    int32_t close_res = 0;        // raw close CQE result, including expected -ECANCELED

    std::array<op_result, max_initial_ops> results;
};

// Derived: was a close actually needed (vs. requested)?
// open_ok == false ⇒ close was not needed regardless of mode.
[[nodiscard]] bool close_needed(const direct_file_flow_state& st) noexcept {
    return st.close_requested && st.open_ok;
}
```

Implementation may differ in layout but must preserve these facts:

- how many user-recorded ops the flow has (`initial_op_count` == open + body count); this bounds `results[]` indexing and is distinct from `expected_cqes` (which counts the close in mode B);
- how many CQEs are expected from the chain (open+body in mode A; open+body+close in mode B);
- how many of those CQEs have been seen;
- which CQE corresponds to direct open;
- whether direct open succeeded;
- whether cleanup was *requested* by the user;
- whether cleanup is *needed* (`close_requested && open_ok`);
- whether cleanup has been emitted as part of the chain (`close_in_chain`, mode B) or submitted standalone (`close_submitted`, mode A);
- whether cleanup is deferred (`close_pending`, mode A only);
- whether a close CQE has been observed and its result;
- every operation result;
- the generation of the slab entry.

Invariant: `close_in_chain == true` means a close SQE was actually emitted in the initial chain and counted in `expected_cqes`. It must never mean only "this chain shape would have been eligible for mode B."

`flow_index` is an index into a preallocated slab, not a hash-map key.

---

## Pending Operation Model

```cpp
struct pending_read {
    void*    buf;
    uint32_t len;
    uint64_t offset;
};

struct pending_write {
    const void* buf;
    uint32_t    len;
    uint64_t    offset;
};

struct pending_open_direct {
    DirectSlot  slot;
    int         dfd;
    const char* path;
    int         open_flags;
    mode_t      mode;
};

struct pending_op {
    flow_op_kind kind;
    link_variant variant = link_variant::then_; // ignored for op[0] (open)

    union {                                    // INVARIANT: trivially-copyable members only
        pending_open_direct open;              // owned variants would require std::variant or
        pending_read        read;              // separate arrays — out of scope for v1
        pending_write       write;
    };
};
```

The byte count for short-I/O detection lives inside `pending_read::len` / `pending_write::len`; there is no separate `requested_size` field, and the emission loop populates `op_result::requested` directly from the active union member.

A real implementation may use a tagged union, variant-like fixed storage, or separate arrays. It must not allocate dynamically in the hot path.

### Borrowed-string type for paths

`const char*` is acceptable but does not document the lifetime expectation. Prefer a typed wrapper:

```cpp
struct borrowed_cstr {
    const char* ptr;   // null-terminated; lifetime per Lifetime Requirements
};

using borrowed_path = borrowed_cstr;
```

`open_direct(...)` takes `borrowed_path`, not raw `const char*`. The wrapper has zero runtime cost and makes the borrow contract visible in signatures.

---

## CQE Tagging

Each submitted SQE in a managed flow must receive `user_data` that identifies:

- flow-state slab index;
- generation;
- operation index;
- operation kind.

Bit layout (64-bit, single allocation unit):

```cpp
struct flow_user_data {
    uint64_t flow_index : 24;
    uint64_t generation : 24;
    uint64_t op_index   : 8;
    uint64_t op_kind    : 8;
};
static_assert(sizeof(flow_user_data) == 8);
```

Bitfields are acceptable here — `user_data` is produced and consumed inside the same translation unit compiled with the same compiler at the same flags. The encoded value is never serialized, never `memcpy`-ed across processes, and never compared against a value produced by a different compiler. Under that constraint the implementation-defined parts of bitfield layout (field order, straddling, signed-bit padding) do not matter. Manual shift/mask encoders are not required unless benchmarks show a measurable improvement over compiler-generated bitfield code.

The CQE path must:

1. Decode `user_data`.
2. Index directly into the flow-state slab.
3. Verify generation.
4. Apply the CQE to that state.

Do not rely on CQE arrival order. Do not use hash-map lookup in the CQE hot path.

---

## Generation Discipline

Slab entries reuse the same `flow_index` over the program's lifetime; the `generation` field distinguishes successive owners.

Rules:

- Generation 0 is reserved for "stale or never-allocated."
- A fresh slab cell starts at generation 0; no live SQE ever carries `generation == 0` in its `user_data`.
- Allocation increments generation. On wrap, the post-increment skips 0:
  ```cpp
  state.generation = (state.generation + 1) & 0xFFFFFF;
  if (state.generation == 0) state.generation = 1;
  ```
- Free does not require a generation bump; the next allocate handles it.

Slab lookup:

```cpp
direct_file_flow_state* try_get(uint32_t flow_idx, uint32_t gen) noexcept {
    if (flow_idx >= kMaxFlows) return nullptr;
    auto& st = cells[flow_idx];
    if (st.generation == 0)        return nullptr;   // never-allocated cell
    if (st.generation != gen)      return nullptr;   // stale (cell reused by a later allocation)
    return &st;
}
```

The lookup catches two conditions:

- **Never-allocated cell** (`st.generation == 0`): caller's `gen` is also nonzero (live SQEs never carry `gen == 0`), so the second check would also reject, but the explicit zero check is defensive and self-documenting.
- **Stale CQE after slab reuse**: the cell was freed and reallocated under a new `gen`; the stale CQE's `gen` no longer matches.

Free does not bump the generation. This is safe **only** under the runtime invariant: `finish_flow` releases the cell only after `seen_cqes == expected_cqes` and (in mode A) the close CQE has been observed. Therefore no live CQE for the just-freed owner can arrive after free, and the next allocation's bump produces a unique `gen` for the new owner.

If CQE overflow or any other path can deliver CQEs after free, this invariant breaks and the lookup will misroute them to the new owner. CQ sizing and overflow handling are the framework's responsibility (*Integration Contract* item 11): the framework must size the CQ ring large enough that managed CQEs are not dropped, and must drain the kernel's overflow list before a flow can be considered stuck.

A 24-bit generation gives ~16M reuses before wrap; with the skip-0 invariant, all 16M-1 values are usable.

The skip-0 rule is preferred over an `occupied` flag because adding such a flag would inflate `slab_cell` by an aligned word for negligible benefit.

### Generation-wrap throughput envelope

24-bit generation wraps after 2^24 - 1 ≈ 16.7M allocations of the same `flow_index`. A stale CQE survives only as long as the kernel takes to deliver it; a colliding `gen` after wrap can misroute that CQE to a new owner.

Safe envelope per `flow_index`:

```text
allocations_per_second_per_index * max_kernel_cqe_latency_seconds < 16.7M
```

For typical `io_uring` workloads this is many orders of magnitude under the limit. If a deployment can exceed it (e.g., extremely high churn on a small `kMaxFlows`), reallocate bits toward `generation` (e.g., 20-bit `flow_index` + 28-bit `generation`) at the cost of a smaller flow slab.

---

## Completion Handling Algorithm

The CQE handler is structured so that `finish_flow` is reached unconditionally once the chain is fully accounted for, regardless of which CQE happened to land last.

```cpp
void flow_runtime::on_cqe(io_uring_cqe* cqe) {
    auto tag = decode_user_data(cqe->user_data);

    auto* st = flow_slab.try_get(tag.flow_index, tag.generation);
    if (!st) {
        handle_stale_or_invalid_cqe(cqe);
        return;
    }

    // Validate op_kind enum value before any branching that depends on it.
    if (!is_valid_flow_op_kind(tag.op_kind)) {
        handle_stale_or_invalid_cqe(cqe);
        return;
    }
    auto kind = static_cast<flow_op_kind>(tag.op_kind);

    if (kind == flow_op_kind::close_direct) {
        // Close branch never indexes results[]; close_res is a separate field.
        // Mode A: expected_cqes counts open+body only; the standalone close CQE
        // arrives AFTER seen_cqes == expected_cqes — a guard here must not fire
        // on that legitimate close. The close branch has its own duplicate check.
        bool close_expected = st->close_in_chain || st->close_submitted;
        if (!close_expected) {
            handle_stale_or_invalid_cqe(cqe);
            return;
        }
        // Mode B's synthesized close lives at op_index == initial_op_count.
        // Mode A's standalone close uses the same encoding (see Recommended
        // Implementation Strategy). Anything else is a corrupted tag.
        if (tag.op_index != st->initial_op_count) {
            handle_stale_or_invalid_cqe(cqe);
            return;
        }
        // Duplicate close CQE: finalization already ran or a second spurious
        // close CQE arrived. Reject to avoid double-finish.
        if (st->close_seen) {
            handle_stale_or_invalid_cqe(cqe);
            return;
        }

        st->close_seen = true;
        st->close_res  = cqe->res;

        if (st->close_in_chain) {
            // Mode B: close counts toward chain CQEs. Fall through to the
            // chain-equality check below; do NOT finalize here, because the
            // close CQE may arrive before the last body CQE.
            st->seen_cqes++;
        } else {
            // Mode A: close is standalone, submitted only after the chain was
            // already accounted for. The close CQE alone finalizes the flow.
            finish_flow(*st);
            return;
        }
    } else {
        // Body / open CQE.
        // Reject duplicates: must not push seen_cqes past expected_cqes.
        if (st->seen_cqes >= st->expected_cqes) {
            handle_stale_or_invalid_cqe(cqe);
            return;
        }
        if (tag.op_index >= st->initial_op_count) {
            // Corrupted or stale tag: index past the user-recorded op count.
            // (Tighter than max_initial_ops; the unused tail of results[] is
            // never legitimately addressed.)
            handle_stale_or_invalid_cqe(cqe);
            return;
        }

        auto& r = st->results[tag.op_index];
        if (r.kind != kind) {
            // Tag's op_kind disagrees with what was recorded at submit time.
            // Treat as corrupted; do not write res into the wrong slot.
            handle_stale_or_invalid_cqe(cqe);
            return;
        }

        st->seen_cqes++;
        r.res = cqe->res;

        if (kind == flow_op_kind::open_direct) {
            st->open_seen = true;
            st->open_res  = cqe->res;
            st->open_ok   = direct_open_succeeded(cqe->res);
        }
    }

    if (st->seen_cqes == st->expected_cqes) {
        on_chain_complete(*st);
    }
}
```

`is_valid_flow_op_kind(uint8_t)` is a small whitelist over the enum's known values:

```cpp
[[nodiscard]] constexpr bool is_valid_flow_op_kind(uint8_t v) noexcept {
    switch (static_cast<flow_op_kind>(v)) {
        case flow_op_kind::open_direct:
        case flow_op_kind::read:
        case flow_op_kind::write:
        case flow_op_kind::close_direct:
            return true;
    }
    return false;
}
```

The bounds-check and consistency rules above are not "belt-and-braces" — corrupted `user_data` (e.g., a SQE-prep bug, a CQE delivered against a freed-and-reallocated cell despite generation discipline, or a mismatched-kind tag from a future code change) must not silently write into the wrong result slot or finalize the wrong flow. Failures route to `handle_stale_or_invalid_cqe`, which the framework defines (typically log + drop).

`on_chain_complete` is the single point where chain-finalization decisions are made:

```cpp
void flow_runtime::on_chain_complete(direct_file_flow_state& st) {
    if (st.close_in_chain) {
        // Mode B: close was part of expected_cqes; its result is already in
        // st.close_res (the close handler set it before falling through).
        finish_flow(st);
        return;
    }

    // Mode A.
    if (!st.close_requested || !st.open_ok) {
        // No close needed (open failed or user did not request cleanup).
        finish_flow(st);
        return;
    }

    if (st.close_submitted) return; // already submitted; close CQE will finalize

    auto* sqe = framework_->try_acquire_sqe();
    if (!sqe) {
        st.close_pending = true;
        framework_->register_deferred_close(st.flow_index, st.generation);
        return;
    }
    submit_close_direct(sqe, st.slot, st.flow_index, st.generation);
    st.close_submitted = true;
    st.close_pending   = false;
}
```

Why this structure:

- Mode B: chain equality (including the close CQE) is the single finalization trigger. Order of CQE arrival does not matter.
- Mode A: chain equality only finalizes the *initial* chain; the runtime then either finalizes immediately (no close needed) or submits the close. The eventual close CQE finalizes from the close branch.

`submit_close_direct` emits a standalone cleanup SQE tagged with `flow_op_kind::close_direct`. In mode A it stands alone with no link flags. In mode B the close is part of the chain emission (see *Recommended Implementation Strategy*) and is the chain's terminal SQE; both `IO_LINK` and `IO_HARDLINK` are cleared.

**Close-CQE `op_index` encoding.** Both modes encode the close SQE's `tag.op_index` as `state.initial_op_count` (i.e., the position immediately past the last user-recorded op). Mode B uses this naturally because the synthesized close is appended at index `b.op_count`. Mode A uses the same encoding for consistency: `submit_close_direct(sqe, slot, flow_index, generation)` internally encodes `op_index = state.initial_op_count`. The `on_cqe` close branch validates `tag.op_index == st->initial_op_count`; a mismatch indicates a corrupted tag.

---

## Cleanup Backpressure (mode A only)

If the SQ is full at the moment the runtime wants to submit `close_direct`, the runtime cannot drop the close. It defers:

1. `try_acquire_sqe()` returns null.
2. The runtime sets `close_pending = true` and calls `framework->register_deferred_close(flow_index, generation)`.
3. The framework retries `flow_runtime::resume_deferred_close(flow_index, generation)` whenever SQ space frees.

```cpp
void flow_runtime::resume_deferred_close(uint32_t flow_idx, uint32_t gen) {
    auto* st = flow_slab.try_get(flow_idx, gen);
    if (!st || !st->close_pending) return;

    auto* sqe = framework_->try_acquire_sqe();
    if (!sqe) return; // still full; framework will call again

    submit_close_direct(sqe, st->slot, st->flow_index, st->generation);
    st->close_submitted = true;
    st->close_pending   = false;
}
```

A flow is not finished — and its slab cell is not freed — until either the close CQE has been seen, or the runtime determined that no close was required (open failed).

Both `close_submitted` and `close_pending` updates inside `resume_deferred_close` happen under the framework's emission serialization. The framework guarantees no concurrent `resume_deferred_close` for the same `(idx, gen)`; the top-of-function check `if (!st || !st->close_pending) return;` handles the redundant-retry case once a successful submission has cleared `close_pending`. The order between setting `close_submitted = true` and clearing `close_pending = false` is not externally observable because the lock spans both writes.

Mode B does not exercise this path: the close is reserved as part of the initial chain's SQE budget, which the framework must guarantee per *Integration Contract* item 3.

### Framework drain protocol

The framework owns the lifecycle of every `register_deferred_close` registration. The contract:

1. For every successful `register_deferred_close(idx, gen)` call, the framework must call `resume_deferred_close(idx, gen)` repeatedly until one of the following resolves the registration:
   - the call submits the close SQE (the runtime sets `close_submitted = true` and clears `close_pending = false`); or
   - the framework's shutdown protocol abandons the registration per item 2 below.

   `resume_deferred_close` is idempotent: redundant calls after submission are no-ops via the `close_pending` guard. "At least once and until resolved" is the correct contract; "exactly once" is not.
2. On framework shutdown or ring destruction, the framework must drain all outstanding deferred-close registrations *before* tearing down the ring. Two acceptable strategies:
   - drain by retry until SQ space is available, submit, and wait for the close CQE; or
   - drain by force-cancel: synthesize an internal "close abandoned" finalization that frees the slab cell without emitting an SQE. The kernel direct slot remains populated; this is acceptable only if the entire ring is being destroyed (the kernel reclaims the direct table on ring close).
3. Failure to drain leaks slab cells and direct-slot leases. There is no flow-layer timeout; the framework is the sole owner of the deferred queue's progress.

A defensive `framework->abandon_flow(idx, gen)` accessor that runs strategy 2 above is acceptable as a v1 escape hatch; it is not required.

### Close failure and lease policy

If a close CQE arrives with `close_res < 0` (other than `-ECANCELED` from cascade), the kernel's view of the direct slot is undefined: the slot may still be populated, partially populated, or empty. Documented failure modes for `io_uring_prep_close_direct` are kernel-OOM and invalid-direct-descriptor; the latter indicates a framework bug.

V1 policy:

1. The flow layer surfaces the close failure via `cleanup_result()`; it does not retry or attempt a second close.
2. The framework must **not** return a slot whose close failed to the generic free pool. Treat the lease as poisoned until external knowledge confirms the slot is empty (e.g., teardown of the entire ring).
3. If the framework's lease tracker cannot represent a poisoned state, log the failure prominently and leak the slot for the lifetime of the ring.

A future revision may add a force-close retry path; v1 prefers conservative leak over double-close.

### No-close ownership

A flow that does not call `close_if_opened()` (`close_requested == false`) emits no cleanup. After `finish_flow`:

- the slab cell is released;
- the kernel direct slot remains populated (assuming open succeeded);
- ownership of the slot lease is the framework's responsibility — the slot is **not** automatically returned to the generic free pool.

Use the explicit no-close form only when a higher layer plans to close the slot via a separate flow or via `flow.open_direct(...)` reuse semantics. The flow layer makes no attempt to detect leaked no-close slots.

---

## CQE Counting Rule

For mode A, `expected_cqes` counts the initial linked chain only.
For mode B, `expected_cqes` counts the initial linked chain plus the in-chain close.

`seen_cqes == expected_cqes` means accounting is complete for the chain that was actually submitted.

Soft-link-cancelled tail operations still count. If `read` fails and `write` is cancelled, the cancelled `write` CQE still contributes to `seen_cqes`. A cancelled operation result is recorded normally as `-ECANCELED`; do not special-case cancellation as "not a real CQE."

---

## Direct Open Success Rule

For explicit direct slot open:

```text
success: cqe.res >= 0
failure: cqe.res < 0
```

Explicit-slot success is usually `res == 0`. Future support for kernel-allocated direct slots would return the allocated index in `res`.

The implementation should not scatter raw success checks. Use a helper:

```cpp
[[nodiscard]] bool direct_open_succeeded(int32_t res) noexcept {
    return res >= 0;
}
```

Normal users receive `op_result` / `flow_result`, not raw `res` interpretation rules.

---

## Fixed-File Flag Rule

Operations issued through the direct-file flow object automatically use fixed-file semantics.

```cpp
f.then_read(buf, len, 0)
```

internally prepares a read against slot `k` and sets `IOSQE_FIXED_FILE`.

The user must not need to write `Fd{k}`, `FixedFile{k}`, or `IOSQE_FIXED_FILE` directly inside `direct_file_flow`. Type choice makes misuse difficult: every read/write method on `direct_file_flow` always means fixed-file read/write against the associated slot.

Lower-level APIs may remain available on raw chains:

```cpp
chain.prep_read(Fd{fd}, ...);        // normal fd
chain.prep_read(FixedFile{k}, ...);  // explicit fixed-file use
```

`close_direct` is the one exception inside `direct_file_flow`: it must not set `IOSQE_FIXED_FILE` because it operates on the direct table, not on a fixed file.

---

## Link Flag Rule

Single source of truth for SQE link flags inside a flow.

Define:

```cpp
constexpr uint32_t LINK_MASK = IOSQE_IO_LINK | IOSQE_IO_HARDLINK;
```

For each emitted SQE i in a flow's chain (open, body ops, and — in mode B — close):

```cpp
sqe[i].flags &= ~LINK_MASK;

bool i_is_terminal = (i + 1 == emitted_count);
if (!i_is_terminal) {
    // The link FROM op[i] TO op[i+1] is governed by op[i+1]'s declared variant.
    sqe[i].flags |= variant_for_link_into(i + 1) == link_variant::hard_
        ? IOSQE_IO_HARDLINK
        : IOSQE_IO_LINK;
}
```

`variant_for_link_into(j)`:

- `j == 1` (link from open into first body): `ops[1].variant`.
- `j` in body range: `ops[j].variant`.
- `j == close-position` (mode B, close auto-appended): the runtime picks based on the predecessor:
  - if predecessor is the open (no body ops): `then_` (so open-fail cancels close).
  - else: `hard_` (so body-fail does not cancel close).

The terminal SQE of each flow's chain has both `IOSQE_IO_LINK` and `IOSQE_IO_HARDLINK` cleared. This must hold even when multiple flows are emitted in one framework submit batch — a terminal SQE from flow A must never accidentally link into the first SQE of flow B. The per-flow terminal-clears rule guarantees this; no cross-flow leakage is possible.

---

## Recommended Implementation Strategy

Two-stage builder:

1. Builder stage records operations in fixed-capacity C++ objects.
2. Submit stage converts recorded operations to SQEs with correct link flags, fixed-file flags, and `user_data` tags.

Internal representation:

```cpp
struct direct_file_builder {
    DirectSlot  slot;

    std::array<pending_op, max_initial_ops> ops;
    uint8_t op_count = 0;

    bool close_requested = false;
    int  err = 0;                  // 0 if valid; builder-side error per Builder Error Surface
};
```

At `flow.submit()`, for each builder:

```cpp
if (b.err != 0) { record_rejection(b); continue; }

bool mode_b  = b.close_requested && mode_b_eligible(b);
uint8_t emitted = static_cast<uint8_t>(b.op_count + (mode_b ? 1u : 0u));
                                 // mode B: chain = open + body + close (close counted)
                                 // mode A: chain = open + body         (close standalone)
                                 // no-close flow: chain = open + body  (close_requested == false)

// Allocate the slab cell BEFORE reserving SQ slots. If the slab is full,
// reject the flow with -ENOSPC without holding any SQ reservation; this keeps
// SQ space available for other flows in the same batch.
auto* state_ptr = flow_slab.try_allocate();  // bumps generation, skips 0 on wrap
if (!state_ptr) {
    record_rejection_with(b, -ENOSPC);
    continue;
}
auto& state = *state_ptr;

if (!framework->reserve_sqe_slots(emitted)) {
    // All-or-nothing: per Integration Contract, the framework MUST guarantee
    // contiguous SQ space for the entire chain, since IOSQE_IO_LINK does not
    // span submit boundaries. If the framework cannot reserve the slots, the
    // flow is deferred or rejected per framework policy; the chain must NOT
    // be partially emitted.
    flow_slab.release(state);                // return the just-allocated cell
    record_rejection_with(b, -EAGAIN);
    continue;
}
// Slab cells are reused; every sticky field from the previous owner must be
// reset explicitly. try_allocate() preserves only flow_index and the
// freshly-bumped generation; everything else is reinitialized here.
state.initial_op_count = b.op_count;          // open + body; bounds for results[]
state.expected_cqes   = emitted;
state.seen_cqes       = 0;
state.slot            = b.slot;
state.open_seen       = false;
state.open_ok         = false;
state.open_res        = 0;
state.close_requested = b.close_requested;
state.close_in_chain  = mode_b;              // true ⇒ close SQE actually emitted in chain
state.close_submitted = false;
state.close_pending   = false;
state.close_seen      = false;
state.close_res       = 0;
// state.results[] is overwritten per-op in the emission loop below; entries
// past `initial_op_count` are not referenced by the CQE handler (the body
// branch bounds-checks tag.op_index < initial_op_count).

for (uint8_t i = 0; i < emitted; ++i) {
    bool is_synthesized_close = mode_b && (i == b.op_count);
    flow_op_kind kind = is_synthesized_close
        ? flow_op_kind::close_direct
        : b.ops[i].kind;

    auto* sqe = framework->acquire_reserved_sqe();
    prep_op(sqe, i, b, is_synthesized_close);
    // prep_op invokes the relevant io_uring_prep_*() helper, which zeroes
    // sqe->flags as part of preparation. The flow runtime relies on this:
    // any framework-default flags written before prep are wiped, and only the
    // bits the runtime ORs in below survive. Out-of-scope flags
    // (IOSQE_ASYNC, IOSQE_BUFFER_SELECT, IOSQE_CQE_SKIP_SUCCESS, etc.) are
    // therefore guaranteed not to leak onto managed-flow SQEs in v1.

    if (uses_fixed_file_fd(kind))
        sqe->flags |= IOSQE_FIXED_FILE;

    sqe->flags &= ~LINK_MASK;
    if (i + 1 < emitted) {
        sqe->flags |= link_flag_for_boundary(i, i + 1, b, mode_b);
    }

    sqe->user_data = encode(state.flow_index, state.generation, i, kind);

    // Initialize the result slot so on_cqe sees a populated kind/requested.
    // The synthesized close result lives in state.close_res, not in results[].
    if (!is_synthesized_close) {
        state.results[i] = op_result{
            .res       = 0,
            .requested = byte_count_of(b.ops[i]),   // 0 for open, len for read/write
            .kind      = kind,
        };
    }
}
```

`mode_b_eligible(b)` is a pure shape predicate over user-recorded ops:

```cpp
[[nodiscard]] bool mode_b_eligible(const direct_file_builder& b) noexcept {
    if (b.op_count < 1)            return false;   // no open
    if (b.op_count == 1)           return true;    // just open + auto-close
    if (b.ops[1].variant != then_) return false;   // first body must allow open-fail cascade
    for (uint8_t i = 2; i < b.op_count; ++i) {
        if (b.ops[i].variant != hard_) return false;
    }
    return true;
}
```

This avoids patching already-emitted SQEs and makes the auto-mode decision local to submit time.

`framework->reserve_sqe_slots(n)` returns `true` only when `n` contiguous SQ slots are available *now*. If it returns `false`, the framework's policy decides whether to defer the flow (queue it for a later submit batch) or reject it. The flow layer never partially emits a chain.

---

## `with_direct_file` Desugaring

```cpp
flow.with_direct_file(k, dfd, path, open_flags, mode, [&](auto f) {
    f.then_read (buf, len, 0)
     .hard_write(out, out_len, header_len);
});
```

desugars to approximately:

```cpp
auto f = flow.open_direct(k, dfd, path, open_flags, mode);

f.then_read (buf, len, 0);
f.hard_write(out, out_len, header_len);

f.close_if_opened();
```

Inside the scoped form, `close_if_opened()` is applied automatically at scope end. There is no API to suppress it inside the scoped form; users who do not want auto-close must use the explicit form and not call `close_if_opened()`.

---

## Lifetime Requirements

The flow API is zero-copy by default. Path and buffer memory passed to SQEs is borrowed; the typed wrapper `borrowed_path` (alias for `borrowed_cstr`) makes this explicit in `open_direct` signatures.

Lifetime depends on what kind of memory it is:

**Paths (open_direct).** The kernel captures path strings during SQE preparation via `getname()`. With `IORING_FEAT_SUBMIT_STABLE` advertised, path pointers must remain valid only until `io_uring_enter` returns for the submit that emitted the open. Without that feature, path pointers must remain valid until the open CQE is observed.

**SQPOLL caveat.** Under `IORING_SETUP_SQPOLL`, user space cannot precisely observe when the kernel polling thread has consumed an SQE's metadata, so the relaxed "valid until submit returns" rule is not safely enforceable for path strings. V1 conservatively requires path lifetime to extend to the open CQE under SQPOLL, regardless of `IORING_FEAT_SUBMIT_STABLE`. Buffer lifetime rules (below) are unaffected — they already extend to the CQE.

**I/O buffers (read/write).** Buffer pointers must remain valid until the corresponding CQE is observed. `IORING_FEAT_SUBMIT_STABLE` does **not** cover memory reachable through SQE pointers — it only stabilizes bytes inside the SQE itself plus prep-captured path strings. Read/write buffers are operated on asynchronously after submit returns; freeing them before the CQE corrupts data on every kernel.

The framework should:

- query `IORING_FEAT_SUBMIT_STABLE` at startup;
- enforce path lifetime via flow-scoped allocators or an explicit copy when the feature is absent;
- require buffer lifetime to extend to the CQE in all cases.

If the framework cannot satisfy the path-lifetime contract, the flow layer must reject submission with `-EOPNOTSUPP` rather than silently fall through and cause use-after-free.

The flow layer does not copy paths or buffers in the default API. Owned variants may be added later (out of scope for v1):

```cpp
f.read_owned(std::vector<std::byte>{...}, 0);
flow.with_direct_file_owned_path(k, dfd, std::string{path}, open_flags, mode, fn);
```

For v1:

```cpp
f.then_read (buf, len, offset);
f.hard_write(buf, len, offset);
```

means borrowed memory; buffers must outlive their CQE.

---

## Short I/O Semantics

For read/write operations, `res >= 0` means kernel success; `0 <= res < requested` is a short I/O.

Short I/O is recorded but not automatically converted to a hard error by the flow layer. It is exposed via `op_result::short_io()` (returns false unless `is_io()`).

### Kernel link interaction

Short read or short write on a soft-linked SQE triggers a cancel cascade: the kernel emits `-ECANCELED` for every downstream linked SQE in the chain. This is a kernel rule, not a user-tunable policy. If a body op must run regardless of partial completion of its predecessor, the user must place a hard-linked op there: choose `hard_*` for the op that must run after a potentially-short predecessor.

`IOSQE_IO_HARDLINK` is the only escape from short-I/O cancellation, and it only crosses *executed* failures; it does not stop a cascade that began upstream of the hard link.

The flow layer does not retry short I/O; that is a higher-layer policy concern.

---

## CQE Skipping Rule

Managed flows must not use CQE-skipping options for operations that participate in lifetime accounting.

Do not allow `IOSQE_CQE_SKIP_SUCCESS` on:

- `open_direct`;
- body operations whose completion contributes to `expected_cqes`;
- `close_direct` (whether mode A or mode B);
- any operation whose result is needed for user-visible flow status.

For v1, managed flows disallow CQE skipping entirely. A future version may support compensated CQE skipping; that mode must adjust `expected_cqes` at build time and track skipped successes via SQE emission counts, not via CQE observation.

---

## Error Semantics

The flow preserves every operation result.

### Boundary terminology

In what follows, "boundary X→Y" means the link flag set on SQE X that governs how X's result affects Y's execution. Per the API convention, the variant chosen for Y (`then_*` or `hard_*`) determines this flag:

- Y declared `then_*` ⇒ X→Y is soft (`IOSQE_IO_LINK`).
- Y declared `hard_*` ⇒ X→Y is hard (`IOSQE_IO_HARDLINK`).

Outcomes:

```text
open fails:
    open result < 0
    body ops: -ECANCELED (cascade from open's IO_LINK to first body)
    close: mode A — not submitted (open_ok == false)
           mode B — cancelled with -ECANCELED in chain

open succeeds; the body cancellation/short rules below depend on the boundary
between each pair of adjacent ops, not on either op's name in isolation.

open ok, read fails or short, boundary read→next is soft:
    read result preserved (< 0 or short_io)
    next: -ECANCELED (cascade)
    everything past 'next' also cancelled (cascade is unconditional once started)
    close: mode A — submitted (open_ok == true)
           mode B — under v1 eligibility rule, mode B requires every read→next
                    boundary to be hard for read past op[1]. A soft boundary
                    here means the chain is mode A, so close is standalone.

open ok, read fails or short, boundary read→next is hard:
    read result preserved
    next runs regardless of read's executed result
    no cascade; close behavior depends on subsequent boundaries

open ok, read ok, write fails:
    write result < 0
    boundary write→close (mode B) is hard by eligibility ⇒ close runs in chain
    mode A: close submitted standalone after chain accounting

all succeed:
    all results successful
    close submitted in mode A; counted in mode B
```

A short `then_*` op cancels the next op exactly the way an outright failed `then_*` op does. The kernel does not distinguish "short" from "failed" for soft-link cascade purposes.

### Cleanup result normalization

Mode A and mode B observably differ for "open failed, no cleanup performed":

- Mode A: runtime declines to submit close. No close CQE.
- Mode B: close was emitted in chain and cancelled by cascade. Close CQE arrives with `-ECANCELED`.

The user did not perform cleanup in either case; the difference is implementation detail of the auto-mode. `flow_result::cleanup_result()` normalizes this so that mode A and mode B return the same observable result for every legal scenario.

The single normalization predicate is `close_needed == close_requested && open_ok`. The canonical implementation lives with the result type; see `flow_result::cleanup_result()` in *Result Model*.

For debugging, the raw close CQE result is available via `flow_result::raw_close_result()` (returns the actual `int32_t` including `-ECANCELED` cases, or `std::nullopt` if no close CQE was observed).

Cleanup failure is reported but does not overwrite the primary body/open failure:

```cpp
[[nodiscard]] const op_result* primary_error(const flow_result& r) noexcept {
    for (auto& op : r.ops) {
        if (op.res < 0) return &op;
    }
    return nullptr;
}
```

---

## Result Model

The result model exposes fixed-capacity spans/views into framework-owned result storage. No heap-owning futures in v1.

```cpp
struct flow_result {
    // ops.size() == initial_op_count == open + body count.
    // The close result is NEVER exposed through ops, regardless of mode.
    // Mode B's chain CQE for the close populates close_raw_res below; mode A's
    // standalone close populates the same field. ops indexes 0..op_count-1.
    std::span<const op_result> ops;            // open + body results only

    bool         close_needed;                 // close_requested && open_ok
    bool         close_in_chain;               // mode B
    bool         close_cqe_seen;               // close CQE was observed
    int32_t      close_raw_res;                // raw close CQE result (or 0)

    [[nodiscard]] bool open_ok() const noexcept {
        return !ops.empty() && ops[0].res >= 0;
    }

    // Normalized: nullopt if no cleanup was needed (regardless of mode).
    [[nodiscard]] std::optional<int32_t> cleanup_result() const noexcept {
        if (!close_needed)        return std::nullopt;
        if (!close_cqe_seen)      return std::nullopt;   // shouldn't happen if close_needed,
                                                         // but defensive
        return close_raw_res;
    }

    // Debug accessor: raw close CQE result regardless of normalization.
    [[nodiscard]] std::optional<int32_t> raw_close_result() const noexcept {
        return close_cqe_seen ? std::optional<int32_t>{close_raw_res} : std::nullopt;
    }
};
```

Delivery mechanism (callback, coroutine, sync) is framework-defined.

Possible later integrations:

```cpp
flow.submit(callback);
co_await flow.submit_async();
flow.submit_and_wait();
```

V1 only requires that result storage exists and preserves all operation results.

---

## API Shape Sketch

```cpp
class ring {
public:
    flow_builder flow();
};

class flow_builder {
public:
    direct_file_flow open_direct(
        DirectSlot     slot,
        int            dfd,
        borrowed_path  path,
        int            open_flags,
        mode_t         mode = 0);

    template <class Fn>
    void with_direct_file(
        DirectSlot     slot,
        int            dfd,
        borrowed_path  path,
        int            open_flags,
        mode_t         mode,
        Fn&&           build_ops);

    [[nodiscard]] uint32_t submit();             // returns count of accepted flows
    [[nodiscard]] std::span<const flow_rejection> rejected_flows() const noexcept;
};

class direct_file_flow {
public:
    direct_file_flow& then_read (void* buf,        size_t len, uint64_t offset);
    direct_file_flow& hard_read (void* buf,        size_t len, uint64_t offset);
    direct_file_flow& then_write(const void* buf,  size_t len, uint64_t offset);
    direct_file_flow& hard_write(const void* buf,  size_t len, uint64_t offset);

    void close_if_opened();                     // no-op on invalid flow

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int  last_error() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
};
```

`DirectSlot` is a typed wrapper, not a raw `int`:

```cpp
struct DirectSlot {
    uint32_t value;
};
```

`borrowed_path` is a typed null-terminated-string borrow (`borrowed_cstr` alias). It carries the same lifetime expectations as the rest of the borrowed-memory API; see *Lifetime Requirements*.

The slot must be leased by the framework before being passed into the flow.

There is no `after_initial_chain()` / chain-complete builder layer in v2; close-mode selection is internal and automatic.

---

## Naming Guidance

Recommended names:

```text
with_direct_file
open_direct
then_read    hard_read
then_write   hard_write
close_if_opened
```

`direct_file_flow` is preferred over `direct_file_builder` for the user-facing object: it represents the logical flow being constructed, not merely an implementation builder.

`flow_builder` remains the name for the object returned by `ring.flow()` because it accumulates one or more flows before submission. (The R3 alternative `flow_batch` was considered; `flow_builder` is kept because it matches the established Java/Rust accumulator naming idiom users will recognize.)

Use `open_flags`, not `flags`, in public signatures. In an `io_uring` API, `flags` alone is ambiguous.

There are no `read()` or `write()` aliases. The four body verbs are exactly: `then_read`, `then_write`, `hard_read`, `hard_write`.

---

## `mode_t` Rule

`mode` is meaningful when `open_flags` contains `O_CREAT` *or* `O_TMPFILE`. Both flag families require a mode. For other open patterns, omit the mode argument or pass `0`.

```cpp
open_direct(slot, dfd, path, open_flags);            // mode = 0
open_direct(slot, dfd, path, open_flags, mode);      // explicit
```

A later API may add `create_direct(...)` for the `O_CREAT|O_RDWR` case but v1 does not need it.

---

## Core Invariants

1. Direct slots are represented by typed objects (`DirectSlot`), not repeatedly passed as raw integers.
2. Slot leasing/allocation is external to this flow layer.
3. The framework guarantees exclusive slot ownership for the flow lifetime.
4. Body operations through the direct-file flow object automatically use `IOSQE_FIXED_FILE`; `close_direct` does not.
5. The initial open/body sequence (and, in mode B, the close) is submitted as one linked chain emitted in a single `io_uring_enter`. `IOSQE_IO_LINK` does not span submit boundaries; the framework reserves all SQ slots for the chain before emission, all-or-nothing.
6. Every chain operation produces an accounted CQE.
7. Cancelled tail CQEs count toward `seen_cqes`.
8. The first body op must be `then_*`. `hard_*` as the first body op makes the flow invalid (`-EINVAL`).
9. Cleanup mode is auto-selected at submit time:
    - mode B (in-chain close) when `close_requested && mode_b_eligible(b)`;
    - mode A (standalone close) when `close_requested && !mode_b_eligible(b)`;
    - no close when `!close_requested`.
10. The user does not select cleanup mode directly. There is no "force mode A" or "force mode B" knob.
11. `close_in_chain == true` means a close SQE was actually emitted in the chain and counted in `expected_cqes`; never merely "would have been eligible."
12. Mode A close is submitted only after all initial-chain CQEs have been observed and only if `close_needed`; mode B close is part of the chain and cancelled by cascade if open fails.
13. Mode A close submission may be deferred via `framework->register_deferred_close` if the SQ is full. The framework owns the lifecycle of every deferred-close registration, including drain on shutdown.
14. CQE order must not be trusted. `finish_flow` is reached unconditionally once `seen_cqes == expected_cqes`, regardless of which CQE arrived last.
15. The CQE handler validates tags before mutating state:
    - `op_kind` must be a known `flow_op_kind` value;
    - body/open CQEs: `op_index < initial_op_count` (tighter than `max_initial_ops`);
    - body/open CQEs: `results[op_index].kind == op_kind` (kind consistency);
    - body/open CQEs: duplicate rejected when `seen_cqes >= expected_cqes`;
    - close CQEs: `op_index == initial_op_count`;
    - close CQEs: unexpected (neither `close_in_chain` nor `close_submitted`) are rejected;
    - close CQEs: duplicate rejected via `close_seen` guard.
    Mode A close CQEs arrive after `seen_cqes == expected_cqes` and are explicitly permitted; the duplicate guard is `close_seen`, not `seen_cqes`.
16. `user_data` tags identify flow slab index, generation, operation index, and operation kind.
17. Generation 0 is reserved; live SQEs never carry `generation == 0`. Allocator increments and skips 0 on wrap.
18. Managed flow operations do not skip CQEs required for accounting.
19. Submit/CQE hot paths do not allocate.
20. State lookup in the CQE path is slab-indexed, not hash-map-based.
21. The scoped syntax desugars to explicit `open_direct` + body + `close_if_opened()` semantics.
22. The builder callback is synchronous and only records operations; the `f` handle must not be held across coroutine suspension.
23. Storage uses `std::array` only; no `std::vector`, no third-party containers, no new dependencies.
24. The builder error surface is allocation-free and exception-free; appending to an invalid flow is a no-op (including `close_if_opened`).
25. `cleanup_result()` returns `nullopt` whenever `!close_needed`, so mode A and mode B are observably equivalent for the open-failure case. `raw_close_result()` exposes the un-normalized close CQE result for debugging.

---

## Minimal First Implementation Target

Implement only this case first:

```cpp
flow.with_direct_file(k, dfd, path, O_RDWR | O_CLOEXEC, 0, [&](auto f) {
    f.then_read (header,  header_len,  0)
     .then_write(payload, payload_len, header_len);
});
```

The implementation must support both this default (mode A) and the hard-tail variant (`hard_write` instead of `then_write`, routing to mode B). The auto-mode-selection logic is part of v1, not v2.

Support:

- one direct open;
- explicit externally leased direct slot;
- one or more dependent fixed-file body operations;
- automatic auto-mode close after the chain (mode B when eligible, mode A otherwise);
- per-operation result storage;
- short-I/O visibility;
- generation-checked slab lookup;
- zero hot-path allocation;
- no drain;
- no CQE skipping;
- no branching inside the body;
- no owned path/buffer helpers yet;
- no internal locking.

After that works, generalize to:

- multiple direct-file flows in one `flow.submit()`;
- framework callbacks;
- coroutine integration;
- allocated direct slots;
- owned path/buffer helpers;
- branching or recovery flows;
- compensated CQE skipping, if worthwhile.

---

## Minimal Test Matrix

Cover these first:

```text
auto-mode selection:
    open + then_read + then_write       → mode A (read→write soft; cascade reaches close)
    open + then_read + hard_write       → mode B (read→write hard; close in chain)
    open + then_read                    → mode B (read→close hard auto)
    open + then_read + hard_write + hard_X → mode B (tail stays hard)
    open + then_read + hard_write + then_X → mode A (trailing soft re-introduces cascade)
    open alone (no body)                 → mode B (open→close soft auto)
    open + hard_read + ...               → invalid (first body must be then_*)
        flow.valid() == false
        flow.last_error() == -EINVAL
        flow.submit() rejects this flow; rejected_flows() contains its index

mode A happy path:
    open + then_read + then_write, all succeed:
        seen_cqes == expected_cqes (open + read + write)
        runtime submits standalone close after chain accounting
        close CQE observed
        flow_result.cleanup_result() == 0

mode A open fails:
    open result < 0
    body CQEs observed as -ECANCELED (cascade from open's soft link)
    runtime does NOT submit close (open_ok == false)
    flow finishes; cleanup_result() == nullopt
    raw_close_result() == nullopt (no close CQE)

mode A read fails (then_read + then_write):
    open ok, read < 0, write -ECANCELED (cascade)
    runtime submits standalone close
    close result preserved in cleanup_result()

mode A read short (then_read + then_write):
    open ok, read.short_io() == true, write -ECANCELED
    standalone close submitted
    caller sees op_result::short_io() == true

mode A write fails (then_read + then_write):
    open ok, read ok, write < 0
    standalone close submitted
    write failure preserved

mode A close_direct fails:
    body/open results preserved unchanged
    cleanup failure surfaces via cleanup_result()

mode A SQ-full at close-submit:
    chain accounting completes
    framework SQ exhausted at on_chain_complete
    close_pending == true; deferred hook registered
    framework drains SQ → resume_deferred_close re-submits
    close_seen → finish_flow
    double-call resume_deferred_close after success: no-op (close_pending was cleared)

mode B happy path (open + then_read + hard_write):
    one io_uring_enter
    expected_cqes counts open + read + write + close (4)
    all CQEs observed; cleanup_result() == close's res

mode B open-fail cascade (open + then_read + hard_write):
    open fails → read, write, close all observed as -ECANCELED
    expected_cqes still satisfied (cancelled CQEs count)
    cleanup_result() == nullopt              (normalized: open_ok == false)
    raw_close_result() == -ECANCELED         (debug visibility preserved)
    no slot population: kernel direct-table entry was never installed;
        framework releases the lease as part of normal flow completion

mode B body-fail tolerance (open + then_read + hard_write):
    open ok, read fails:
        read→write boundary is hard (hard_write was chosen) → write runs
        write→close boundary is hard (auto in mode B) → close runs
        all four CQEs observed; close runs and cleanup_result() reflects close's res
    open ok, read short:
        same as read fails: hard boundary → write runs; close runs
    open ok, read ok, write fails:
        write→close hard → close runs anyway
        cleanup_result() reflects close's res

mode B close-CQE-arrives-before-last-body-CQE:
    enforce that finish_flow is called regardless of CQE arrival order:
        deliver close CQE, then last body CQE (out of order vs. submission)
        flow MUST finish (regression test for the chain-equality finalizer rule)

mode B vs mode A observable equivalence on open failure:
    auto-mode is deterministic per shape; pick a shape pair that exercises each:
        mode B shape (e.g., open + then_read + hard_write) with open-fail
        mode A shape (e.g., open + then_read + then_write) with open-fail
    both must satisfy:
        cleanup_result() == nullopt
    raw_close_result() differs by mode (A: nullopt; B: -ECANCELED) and is the
    debug accessor only — it is not the user-facing equivalence point.
    There is no force-mode knob; tests must vary shape, not mode.

generation discipline:
    fresh slab cell has generation 0
    after first allocate, generation == 1
    free does not bump; subsequent allocate post-increments and skips 0
    stale CQE with mismatched generation routed to error handler
    no live CQE ever carries generation == 0
    bounds check: CQE with op_index >= initial_op_count (body branch) routed to error handler
    bounds check: CQE with op_index in [initial_op_count, max_initial_ops) also rejected
        (initial_op_count is the tighter runtime bound; max_initial_ops is the compile-time limit)

multiple flows in one submit:
    terminal SQE of each flow has both IO_LINK and IO_HARDLINK cleared
    flow A's last SQE never links into flow B's first SQE
    SQ-slot reservation covers every flow's full chain length (open + body + close in mode B)
    if framework cannot reserve slots for a flow, that flow is recorded in
        rejected_flows() with -EAGAIN; other flows in the batch still submit

builder error surface:
    appending more than (max_initial_ops - 1) body ops past the open sets
        last_error() == -ENOBUFS (max_initial_ops counts open + body together)
    hard_read as op[1] sets last_error() == -EINVAL
    subsequent appends and close_if_opened() are no-ops on invalid flow
    submit() skips invalid flows; rejected_flows() lists them
    builder rejects len > INT32_MAX with last_error() == -EOVERFLOW
        (cqe->res is int32_t; successful completions must fit non-negative int32_t)

framework drain on teardown:
    register_deferred_close called for flow F
    framework shutdown: drain or abandon protocol invoked
    no slab cell leaked; no slot lease leaked

mode A close CQE finalizes after chain accounting:
    open + then_read + then_write, expected_cqes == 3
    open/read/write CQEs land: seen_cqes → 3 == expected_cqes
    on_chain_complete submits standalone close; close_submitted = true
    close CQE arrives: seen_cqes(3) >= expected_cqes(3) but kind == close_direct
        → duplicate-body guard does NOT fire (close branch, not body branch)
        → close_expected == true; op_index == initial_op_count; close_seen == false
        → finish_flow runs; cleanup_result() populated
    (regression: guard in wrong place would drop this CQE and leak the slab cell)

mode A duplicate body CQE rejected after chain complete:
    after seen_cqes == expected_cqes, a stray body-tagged CQE arrives
    body branch: seen_cqes >= expected_cqes → routed to handler
    state unchanged; finish_flow not re-invoked

CQE tag validation (defensive, against corrupted user_data):
    op_kind not in known enum values → routed to handle_stale_or_invalid_cqe
    body CQE with op_index in [initial_op_count, max_initial_ops) → routed to handler
    body CQE with op_index >= max_initial_ops → routed to handler
    body CQE with tag op_kind != results[op_index].kind → routed to handler
    duplicate CQE arriving after seen_cqes == expected_cqes → routed to handler
    close CQE with op_index != initial_op_count → routed to handler
    close CQE for a flow with !close_in_chain && !close_submitted → routed to handler
    in every case above: state is unchanged, finish_flow is not called

explicit no-close ownership:
    open succeeds, no close_if_opened():
        flow finishes; cleanup_result() == nullopt
        framework slot lease NOT returned to generic pool
        (caller is responsible per "No-close ownership" rule)
    open fails, no close_if_opened():
        flow finishes; cleanup_result() == nullopt
        framework slot lease may be returned to generic pool
        (no kernel direct-table entry was installed)

close failure poisons slot lease:
    mode A or mode B, open ok, body ok, close CQE has res < 0 (not -ECANCELED):
        cleanup_result() == close's res (negative)
        framework slot lease marked poisoned; not returned to generic pool
        (per "Close failure and lease policy")

length edge cases:
    len == 0: accepted, single CQE with res == 0 (zero-length read/write is legal)
    len == INT32_MAX: accepted, results[i].requested == INT32_MAX
    len == INT32_MAX + 1 (size_t overflow case):
        builder rejects with last_error() == -EOVERFLOW

slab allocation failure:
    flow_slab.try_allocate() returns null:
        flow rejected with -ENOSPC; rejected_flows() lists it
        no SQ slots reserved (slab alloc precedes reservation)
        framework slot lease retained by framework per "Slot-lease ownership for rejected flows"

rejected_flows() lifetime:
    span returned by rejected_flows() valid until next submit() or builder mutation
```

Ring-full behavior, SQ backpressure, retry policy, and cross-thread submission are tested at the framework/ring scheduler layer, not in this flow abstraction.
