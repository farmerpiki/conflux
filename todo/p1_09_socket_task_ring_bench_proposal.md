# P1-09 Benchmark Proposal: FileReader → SocketTaskRing Migration

> State note (2026-05-16): implementation coverage landed. Keep this as
> benchmark rationale only; remaining work is evidence/measurement, not missing
> `SocketTaskRing` API surface.

Date: 2026-05-10
**Status: implemented for `tcp_increment_coro_bench`.** Steps 1+3 landed in e3f1038; async server + `str/parallel_4` landed in 9851640. `tcp_socket_task_bench` was deleted. The legacy standalone `tcp_parallel_coro_bench` was deleted; the planned N=4 benchmark coverage lives in `str/parallel_4`.

## Problem with current tcp_socket_task_bench

`tcp_socket_task_bench` vs `tcp_increment_coro_bench` is not a fair
comparison:

- Both use an identical single-threaded blocking server — the server is
  not the thing under test but its throughput caps both.
- Server accepts one connection at a time (sequential). Measures
  connect+teardown overhead more than steady-state I/O.
- No multishot accept, no multi-threaded server, no pipelined clients.
- Different binary names → compare-bins only works because variants were
  renamed to match; the benchmark identity (what's actually being
  measured) is ambiguous.
- Doesn't test the path that matters most: the HTTP server's async accept
  loop, where `SocketTaskRing` is already the runtime but the bench
  client still uses blocking sockets.

## What a fair comparison looks like

Replace `FileReader` socket I/O with `SocketTaskRing`/`TcpStream` in
`tcp_increment_coro_bench` itself, adding conditions rather than a
separate binary. The before/after should cover:

| Dimension | Before (FileReader) | After (SocketTaskRing) |
|---|---|---|
| client style | callback, coroutine | callback, coroutine |
| server style | blocking single-thread | blocking single-thread, async SocketTaskRing |
| accept style | one accept per conn | blocking loop, multishot accept |
| parallelism | 1 client | 1 client, N=4 clients on separate connections |

The four `(before, after) × (callback, coroutine)` pairs land in the
same binary with the same server, removing the thermal and setup
differential.

## Scope

### In scope

1. **Extend `tcp_increment_coro_bench`** to add SocketTaskRing client
   variants alongside existing FileReader variants. Server stays
   blocking for the first pass (apples-to-apples isolation of client
   I/O path).

2. **Add async server variant** using `SocketTaskRing`-based accept loop
   with multishot accept (single ring, single thread). Pair with both
   FileReader and SocketTaskRing clients.

3. **Add N=4 parallel clients** variant: four independent connections
   interleaved on one ring (coroutine style only — callback doesn't
   compose). Measures ring contention overhead.

4. **Standalone `tcp_parallel_coro_bench` was deleted.** The planned N-parallel coverage now lives in `str/parallel_4`, so there is no separate benchmark to re-enable for P1-09 acceptance.

5. **compare-bins / recorded benchmark gate**: run before/after on the same binary with
   `--compare` (same tree, two presets, or via config flag). This
   avoids variant-name aliasing hack.

### Out of scope

- TLS bench (`tls_tcp_increment_coro_bench`) — needs
  TLS-over-SocketTaskRing, which is a separate design.
- DB benches — libpq socket layer is not SocketTaskRing-aware.
- `file_copy_coro_bench` — file I/O, no socket equivalent.

## Variant matrix

```
tcp_increment_coro_bench --bench-info → name: "tcp_increment"
configs:
  default         → --iterations 200 --warmup 50  (existing)
  parallel_4      → --iterations 200 --warmup 50 --clients 4

variants (same binary, same config):
  fr/callback          FileReader, blocking server, callback
  fr/coroutine         FileReader, blocking server, coroutine
  str/callback         SocketTaskRing, blocking server, callback
  str/coroutine        SocketTaskRing, blocking server, coroutine
  str/async_callback   SocketTaskRing, async server, callback
  str/async_coroutine  SocketTaskRing, async server, coroutine
  str/parallel_4       SocketTaskRing, async server, 4 parallel coros
```

The `fr/*` variants are the existing code (renamed from `callback` /
`coroutine`). `str/*` are new.

## Implementation plan

### Step 1 — SocketTaskRing client variants (DONE — e3f1038, 2026-05-10)

`str/callback` and `str/coroutine` variants landed in `tcp_increment_coro_bench`.
Uses production `block_on_socket_task` (not a custom event loop). compare-bins gate
passed: callback +1.7%, coroutine -0.5% vs FileReader baseline — both within ±2%.

`tcp_socket_task_bench` deleted: absorbed by `str/*` variants; used a custom
`block_on_ring` event loop (not the production path) and had only 2 variants vs 4.
`tcp_increment_coro_bench` is strictly more complete and tests the correct path.

### Step 3 — rename existing FileReader variants (DONE — e3f1038, 2026-05-10)

`callback` → `fr/callback`, `coroutine` → `fr/coroutine`.

---

### Step 2 — async server using SocketTaskRing (DONE — 9851640)

Implemented as `str/async_callback` and `str/async_coroutine` variants in `tcp_increment_coro_bench` using the `SocketTaskRing` accept/server path.

Current implementation shape:

```text
TcpListener listener{TcpListenerOptions{...}}
SocketTaskRing ring{SocketRawRing{&raw}, completion_table, pack_user_data}
tcp_accept_multishot(listener, ring, {}, handler).detach()
```

- Accepted streams run through `serve_one_async`.
- Protocol stays the same: read line, parse, increment, write.
- Accept style uses the landed `tcp_accept` / `tcp_accept_multishot` coroutine API.

### Step 3 — N parallel clients (DONE — 9851640)

Implemented as `str/parallel_4` plus the `parallel_4` benchmark config (`--clients 4`).

```
Task<u64> parallel_coro_loop(SocketTaskRing& ring, u16 port,
                             u32 n_clients, SZ iters, u64 start)
```

- Spawn `n_clients` coroutines, each with its own `TcpStream`.
- Drive all N on the same ring via `join_all`.
- Report total ns / (n_clients × iters) for per-round-trip comparison.

### Step 4 — compare gate (still measurement work, not missing implementation)

```sh
PGURI=... BENCH_REPS=10 scripts/bench_record.sh --compare-bins \
  "fr:/path/to/tcp_increment_coro_bench" \
  "str:/path/to/tcp_socket_task_bench"
```

Wait — wrong. Since both variants live in the same binary after Step 3,
use `--compare` on same-tree presets, or a config flag:

```sh
PGURI=... BENCH_REPS=10 ONLY_BENCH=tcp_increment \
  scripts/bench_record.sh [run-name]
```

Record one run. Compare `fr/*` vs `str/*` rows in SQL directly.

## Source-state answers

1. **`tcp_listen` / listener accept API** — resolved by P1-09a: `tcp_accept` and `tcp_accept_multishot` landed.

2. **Multishot accept wrapper** — resolved; socket coroutine tests cover the multishot accept path, including a 100-connection case.

3. **N-parallel benchmark coverage** — resolved inside `tcp_increment_coro_bench` as `str/parallel_4`; standalone `tcp_parallel_coro_bench` was deleted and is not needed for this proposal.

4. **bench-info JSON** — `bench_record.sh` reads all NDJSON rows from one run; `parallel_4` coverage lives in the normal recorder path.

5. **`tcp_socket_task_bench` fate** — deleted (2026-05-10). `tcp_increment_coro_bench` covers all variants using the production `block_on_socket_task` path.

## Expected outcomes

| hypothesis | expected result |
|---|---|
| `str/callback` vs `fr/callback` | within ±2% — SocketTaskRing callback is equivalent; any overhead is io_uring submission path |
| `str/coroutine` vs `fr/coroutine` | within ±2% — same |
| `str/async_coroutine` vs `fr/coroutine` | within ±5% — async server adds one extra ring round-trip per accept |
| `str/parallel_4` vs `str/coroutine` × 4 | parallel: lower per-conn latency due to batching; or higher due to ring lock contention |

Results outside ±5% on the first two are a red flag requiring
investigation before declaring the migration safe.

## Files touched

- `benchmarks/tcp_increment_coro_bench.cxx` — add SocketTaskRing client
  variants, async server variant, parallel variant; rename `fr/*`
- `src/socket_io/socket_io_coro.cxx` — `tcp_accept` / `tcp_accept_multishot` landed via P1-09a
- `src/socket_io/socket_io.cxx` — accept submission helpers landed via P1-09a
- `benchmarks/CMakeLists.txt` — update link deps for
  `conflux_tcp_increment_coro_bench` (add `conflux_socket_io`)
- `benchmarks/tcp_socket_task_bench.cxx` — delete or demote to
  non-recorded target after merge

## Non-goals

- Do not change what the HTTP server benchmarks measure.
- Do not touch DB or TLS benches.
- Do not change `bench_record.sh` schema or SQL.
