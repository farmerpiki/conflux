# Conflux JSON — Open Items

Post-v16. All v16 items (A–E) shipped in commit 26839f6.

---

## v1-era items (low effort, worth landing before merge to main)

**FI-1 — Build-failed sentinel**
After probe-cap hit, publish `(ObjHashTable*)(1)` sentinel via CAS instead of
leaving `hash_idx_raw = nullptr`. Prevents repeated re-attempts on adversarial
repeat-lookup patterns. Cost: ~4 lines, 3 call sites.
Gate: land only if benchmarks show adversarial repeat-lookup cost.

**FI-3 — Locale singleton intentional-leak comment**
Add comment at `CLocaleHolder` singleton explaining it is never `freelocale`'d
intentionally (static-destruction-order hazard). Prevents future cleanup patches.

**FI-4 — SSS catch-site message hygiene**
`catch (...)` in `warm_member_index` maps to `constraint_violation` with no
message. Pass a descriptive string to `JsonError` at that site to clarify it is
an internal-error mapping, not a caller-input problem.

**FI-6 — Number-lexeme length limit**
4 KiB slow-path cap is a copy budget; adversary can still send megabyte-long
number tokens that the tokenizer scans. Add `max_number_size` constant (e.g.,
1 KiB default) that rejects at tokenize time before any copy.

**FI-9 — VVV span precondition assert**
Before `object_members.subspan(node.off, node.len)` in internal callers, assert
`node.kind == NodeKind::object`. Cheap invariant guard.

**FI-10 — Lower default max_nesting_depth**
Consider dropping from 512 to 256 for fiber/coroutine runtimes with tiny stacks.
Currently 512 frames × 24 bytes = 12 KiB, which is large for 64 KiB stacks.
Policy decision; user can always opt up.

---

## v2 design work

**FI-5 — TTT remainder: deferred-range-error for lexemes > 4 KiB**
v16 Item A fixed the ≤ 4 KiB range-error case. Lexemes > 4 KiB still hit
conservative overflow classification (documented TTT deviation). v2 could raise
the cap or add a second deferred-parse path for those.

**FI-11 — Single-allocation ObjHashTable**
Custom `operator new` with flexible-array suffix for one allocation per table.
v16 preamble confirmed already implemented as such; verify if this is still open.

**FI-16 — Stage-2 Frame stack heap conversion**
N/A — Phase 3 made the parser recursive; Frame struct eliminated. No stack to
convert.

---

## Review decisions (carry forward, not to re-litigate)

See `conflux-json-review-decisions.md` archived notes — fixed v1 decisions
include: immutable Document post-parse, no permissive duplicate mode, strict
numeric path default, Builder in v1 / Editor deferred, no streaming.
