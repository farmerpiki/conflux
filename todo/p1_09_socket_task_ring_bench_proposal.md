# P1-09 Benchmark Proposal: FileReader → SocketTaskRing Migration

Date: 2026-05-10
**Status: implemented for `tcp_increment_coro_bench`.** Steps 1+3 landed in e3f1038; async server + `str/parallel_4` landed in 9851640. `tcp_socket_task_bench` was deleted. The legacy standalone `tcp_parallel_coro_bench` remains disabled, but the planned N=4 benchmark coverage now lives in `str/parallel_4`.

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

4. **Re-enable `tcp_parallel_coro_bench`** once `co_spawn` is available.
   Until then, implement the N-parallel case manually with join_all.

5. **compare-bins gate**: run before/after on the same binary with
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

```
Task<void> async_server_loop(SocketTaskRing& ring, u16 port, atomic_flag& stop)
```

- `tcp_listen(ring, port)` → `ListenSocket`
- Loop: `co_await listen.accept()` → `TcpStream` → `serve_one_async`
- `serve_one_async`: read line, parse, increment, write — same protocol.
- Accept style: start with blocking accept wrapped in `co_await`, then
  add multishot accept variant once the async path is validated.

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

## Questions before implementation

1. **`tcp_listen` / `ListenSocket`** — does this exist in
   `conflux.socket_io.coro`, or does it need to be added?
   (Grep: only `tcp_connect` is exported. `ListenSocket` is not.)
   → Need to add `tcp_listen` + `TcpListener::accept()`.

2. **multishot accept** — `IORING_OP_ACCEPT` with multishot flag. Does
   `socket_io.cxx` export `submit_multishot_accept`? (HTTP server uses
   it directly.) → Need to expose or wrap for bench use.

3. **join_all for N coroutines** — `conflux.work` exports `join_all_n`.
   Confirm it works with `SocketTaskRing`-driven tasks on the same ring
   without `submit_on_owner` complications.

4. **bench-info JSON** — adding a `parallel_4` config with `--clients 4`
   requires the bench binary to output 7 variant rows per invocation.
   Confirm `bench_record.sh` handles multiple rows from one run
   correctly (it does — it reads all NDJSON lines).

5. **`tcp_socket_task_bench` fate** — ✅ deleted (2026-05-10). `tcp_increment_coro_bench`
   covers all variants using the production `block_on_socket_task` path.

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
- `src/socket_io/socket_io_coro.cxx` — add `tcp_listen` / `TcpListener`
  if not already present
- `src/socket_io/socket_io.cxx` — export `submit_multishot_accept`
  wrapper if needed by bench
- `benchmarks/CMakeLists.txt` — update link deps for
  `conflux_tcp_increment_coro_bench` (add `conflux_socket_io`)
- `benchmarks/tcp_socket_task_bench.cxx` — delete or demote to
  non-recorded target after merge

## Non-goals

- Do not change what the HTTP server benchmarks measure.
- Do not touch DB or TLS benches.
- Do not change `bench_record.sh` schema or SQL.
