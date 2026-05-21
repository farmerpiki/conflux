# Maintainer Internals

This guide is a compact pre-edit checklist for runtime and HTTP engine changes.

## Runtime Rules

These rules define ring ownership, buffer ownership, diagnostic policy, and each
component boundary before code is edited.

- Ring ownership is single-owner: submit, reap, and mutate per-ring state on the
  owning runtime thread unless a type explicitly documents cross-thread handoff.
- Connection lifetime is guarded by fd plus generation. Any completion, timeout,
  or cancellation that observes a stale generation must be ignored.
- Send/recv state machines own their in-flight buffers until the matching CQE or
  zero-copy notification closes the lifetime.
- Cancellation is best-effort. Public cancellation APIs request closure and
  unblock waiters; CQEs may still arrive and must pass generation checks.
- `conflux::detail::small_move_only_function` stores its heap/inline state in a
  low-bit tag on the type-erased manager function pointer. This is intended as a
  GCC/Clang/Linux implementation detail: mainstream AArch64 and RISC-V ABIs
  should have sufficient function-pointer alignment, but unusual ARM32,
  sanitizer, or capability-pointer targets may need a non-tagged fallback.
- Buffer ownership is explicit: borrowed request views cannot outlive dispatch,
  registered buffers return through their pool lease, and mapped/file buffers
  remain pinned until the final send completion.
- Zero-copy notification lifetime is separate from send CQE lifetime. Do not
  recycle user-visible storage until the notification path releases it.

## Diagnostics

- Reusable source should use internal diagnostic APIs. Low-level code may use a
  local POSIX `write(2)` sink only when importing `conflux.utils` would create a
  target/component boundary violation.
- Diagnostics that users grep should carry stable codes such as
  `http.route.duplicate`, `http.reject.header_block_too_large`,
  `json.decode.type_mismatch`, `config.unknown_key`, and
  `package.feature_disabled`.
- Coroutine/task frame allocation changes need tests that prove cancellation,
  exception propagation, and result lifetime remain intact.

## Component Boundaries

- `core`, `types`, `json`, and `file_io_sync` must not pull runtime, liburing,
  HTTP/TLS, PostgreSQL, or optional protocol dependencies.
- `runtime` owns io_uring/work scheduling. HTTP server/client components may
  depend on it, but first-contact docs should link the public `conflux::http`
  target rather than private `conflux.net.*` modules.
- DB components are optional. DB-disabled header installs must not ship `db` or
  `pg` generated headers and package smoke must not require libpq.
