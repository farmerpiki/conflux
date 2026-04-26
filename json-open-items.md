# Conflux JSON — Open Items

Post-v16. All v16 items (A–E) shipped in commit 26839f6.

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
