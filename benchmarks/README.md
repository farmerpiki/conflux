# Benchmarks

`conflux_benchmarks` is a dedicated benchmark target for focused, repeatable
in-process performance work. It is intentionally scoped to routing, header
lookup, middleware composition, and compression paths, so the numbers are not
polluted by sandbox/kernel/io_uring variability.

## Perf presets

Use the `perf-*` presets for benchmark recording and profiling. They build only
benchmarks, disable sanitizers, keep debug symbols via `RelWithDebInfo`, and do
not enable LTO. Sanitizer/debug presets intentionally do not build benchmark
targets; sanitizer binaries are correctness artifacts, not performance artifacts.
Use the older `release-*` presets only when you explicitly want to compare
production-style codegen.

```sh
cmake --preset perf-clang-libcxx
cmake --build --preset perf-clang-libcxx --target conflux_record_benches
```

or:

```sh
cmake --preset perf-gcc-stdcxx
cmake --build --preset perf-gcc-stdcxx --target conflux_record_benches
```

Focused debug builds are still available when you need instrumentation or a
debugger, but do not use sanitizer/debug presets for performance conclusions.

Import-surface compile probes are intentionally not part of `conflux_record_benches`;
time their build targets with a cold build directory when evaluating public-surface
or umbrella-module changes:

```sh
/usr/bin/time -v cmake --build --preset perf-clang-libcxx --target conflux_import_http_probe
/usr/bin/time -v cmake --build --preset perf-clang-libcxx --target conflux_import_umbrella_probe
```

P2996 reflection benchmarks use the dedicated release lane:

```sh
cmake --preset release-p2996-gcc
cmake --build --preset release-p2996-gcc --target conflux_json_reflect_bench
```
When `CONFLUX_ENABLE_CPU_DISPATCH=ON`, the optional
`conflux_cpu_dispatch_impl_bench` target compares the scalar fallback,
compiled ISA fastpath, and public dispatch wrapper for the small kernels used by
runtime dispatch. It is not created when CPU dispatch is disabled, so non-dispatch
builds keep the same benchmark target set.

```sh
cmake --build --preset perf-clang-libcxx --target conflux_cpu_dispatch_impl_bench
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_cpu_dispatch_impl_bench --json
```


Build all perf-lane benchmark binaries with:

```sh
scripts/run-perf-matrix.sh
```

## Running individual binaries

```sh
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_benchmarks
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_benchmarks --list
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_benchmarks --filter router
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_benchmarks --iterations 50000
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_benchmarks --format csv
/tmp/<repo>/perf-clang-libcxx/benchmarks/conflux_benchmarks --csv   # alias for --format csv
```

## Bench binary contract

All recordable `conflux_*bench*` binaries implement a standard interface:

- `--bench-info` — prints a JSON descriptor and exits 0; used by
  `scripts/bench_record.sh` for auto-discovery.
- `--json` — outputs NDJSON in the standard shape:
  `{"config":"","variant":"","iterations":N,"total_ns":N,"ns_per_iter":X}`.
  Benchmarks may add extra fields. JSON serde benchmarks emit
  `allocations_per_iter` and `allocated_bytes_per_iter` for allocation-aware
  DOM/direct comparisons.

`--bench-info` JSON shape:

```json
{
  "name":    "logical_bench_name",
  "parser":  "standard|file_copy",
  "configs": [
    {
      "name": "cfg",
      "extra": {},
      "target_ms": 500,
      "max_iterations": 0,
      "calibration_iterations": 16,
      "calibration_min_sample_ms": 50,
      "args": ["--iterations", "0"],
      "reps": 2
    }
  ]
}
```

For standard parser configs, `--iterations 0` is a recorder-only request for
calibration. `scripts/bench_record.sh` runs a short probe, reruns calibration
when the probe is below `calibration_min_sample_ms`, then replaces
`--iterations 0` with a measured count derived from `target_ms`. Counts are
rounded to two significant digits: 500ms targets round up because 500ms is a
minimum, while larger targets round down. `max_iterations: 0` means uncapped.
The calibration uses the sum of all emitted row `ns_per_iter` values, so
multi-variant binaries target the whole launch rather than giving every row a
full target duration. Fixed-duration load/congestion/tail configs should keep
explicit `--duration`/fixed arguments and omit `--iterations 0`.

Parsers:

- `standard` — NDJSON `variant,iterations,total_ns,ns_per_iter`; recorded via
  `record_with_reps`.
- `file_copy` — custom parser; configs come from `--bench-info` JSON.

To add a new bench:

1. Implement `--bench-info` and `--json` in the binary.
2. Add the CMake target to `_record_targets` in `benchmarks/CMakeLists.txt`.

## Recording runs

Prepare the database once:

```sh
createdb conflux_bench || true
psql postgres://postgres@localhost/conflux_bench -f scripts/bench_db_migrate.sql
```

Record both default perf presets:

```sh
PGURI=postgres://postgres@localhost/conflux_bench \
BENCH_ARTIFACT_DIR=/tmp/conflux-bench/manual-001 \
scripts/bench_record.sh manual-001
```

Default recorder behavior:

- `BENCH_PRESET="perf-clang-libcxx perf-gcc-stdcxx"`.
- Every preset is built, recorded, and deleted before the next preset unless
  `KEEP_BUILD=1` is set.
- Every run writes a `manifest.json`, captured `--bench-info` descriptors, copied
  `CMakeCache.txt` files, configure/build logs, and raw NDJSON under
  `BENCH_ARTIFACT_DIR`.
- Recorder preflight rejects wrong preset shapes unless an explicit waiver env is
  set. Required normal shape: `perf-*`, `RelWithDebInfo`, benchmark-only, no LTO,
  no sanitizers.
- The recorder fails when a benchmark produces zero valid NDJSON result rows,
  instead of inserting an empty run.
- `BENCH_REPS=N` controls default repetitions; per-config `--bench-info` `reps`
  overrides it for expensive benches.
- `BENCH_PIN_CPUS=0-3` wraps each benchmark launch in `taskset -c 0-3`.
- `BENCH_ITERATIONS_FROM_RUN_ID=ID` reuses prior stable iteration counts so
  candidate-vs-baseline runs keep fixed inputs, including a derived warmup when
  the benchmark config did not specify one.
- `BENCH_TARGET_MS`, `BENCH_MAX_ITERATIONS`, `BENCH_MIN_ITERATIONS`,
  `BENCH_CALIBRATION_ITERATIONS`, and `BENCH_CALIBRATION_MIN_SAMPLE_MS` provide
  defaults for configs that use `--iterations 0`. Most normal configs target
  short sub-2-second launches; congestion, slow-consumer, tail-latency, and
  duration-based load configs stay explicit because longer wall time is part of
  what they test.

Focused component commands:

```sh
# HTTP/server path
ONLY_BENCH=http_app_path BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-app-path-local
ONLY_BENCH=http_adversarial BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-adversarial-local
ONLY_BENCH=http_server BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-server-local
ONLY_BENCH=http_server_concurrency BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-server-concurrency-local
# The descriptor records both smoke and 30s/5s-warmup tail-proof configs.
ONLY_BENCH=http_server_concurrency BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-server-concurrency-tail-local
ONLY_BENCH=slow_consumer_backpressure BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh slow-consumer-backpressure-local
ONLY_BENCH=send_zc BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh send-zc-threshold-local

# File/runtime path
ONLY_BENCH=file_copy_coro BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh file-copy-local
ONLY_BENCH=storage_read BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh storage-read-nvme
ONLY_BENCH=kernel_state_synthetic BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh kernel-state-local
ONLY_BENCH=db_protocol_synthetic BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh db-protocol-local
ONLY_BENCH=tls_mem_bio BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh tls-mem-bio-local

# Worker/runtime path
ONLY_BENCH=work BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh work-local
ONLY_BENCH=task_chain_composition BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh task-chain-local
ONLY_BENCH=workpool_enqueue_dequeue BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh workpool-local
```

`http_app_path` is the pure user-space framework-cost-floor benchmark. It runs
HTTP/1 request parsing, request view construction, router dispatch, middleware
composition, response construction, and `format_response` serialization without a
live socket or `io_uring` round trip. Configs cover exact routes, parameter
routes, 404, middleware depth 1/4/16, small/medium JSON responses, POST body
parse-only, and POST 4 KiB echo-size. Treat these rows as `micro/user-space`; use
live HTTP rows only to see how kernel/network costs hide or amplify the floor.


`http_adversarial` is the pure user-space HTTP parser/adversarial-load suite.
It covers many small headers, large headers near the aggregate limit, invalid
request lines, duplicate `Content-Length`, `Content-Length` plus
`Transfer-Encoding`, many tiny chunked frames, late malformed chunk framing, late
body-limit crossing, and slowloris-style incomplete headers. Rows emit parser
status, structured reject reason, bytes consumed before rejection/completion,
parsed header count, decoded body bytes, CPU ns/request, and allocation counters.
Treat these rows as `micro/user-space-adversarial`; they are not socket timeout
proof, but they expose the parser/security cost floor before live-kernel rows.

`http_server_concurrency` is the live HTTP concurrency/tail-latency suite.
Short rows default to `--duration 1 --warmup 0` and are labeled
`live-kernel-sanity`; use `--duration 30 --warmup 5` or longer for
`end-to-end-proof` rows. Each row emits p50/p90/p99/p999/max latency, errors,
timeouts, CPU user/sys/total time, CPU utilization, voluntary/involuntary
context switches, fd/RSS start/end, sampled RSS high-water, pressure-event
high-water, and SQ/CQ overflow counters. The `queue_depth_high_water` field is
currently sourced from the cumulative HTTP pressure-event counter because the
server does not yet expose an instantaneous response-queue gauge.

`slow_consumer_backpressure` is the live overload/backpressure suite. It keeps
slow clients attached while the server sends large responses, drives SSE channels
through `drop_newest`, `drop_oldest`, and `disconnect` overflow policies, and
forces WebSocket handoff pressure with a saturated work pool. Rows emit duration,
connection count, bytes read versus expected bytes, estimated unread bytes, RSS/FD
deltas, latency samples where available, HTTP pressure counters, SSE overflow
counters, work-pool enqueue/full counters, and SQ/CQ overflow counters. Rows are labeled `live-kernel-sanity`; treat the short defaults as smoke-quality
backpressure-proof scaffolding. Use longer `--duration` and higher `--connections`
on dedicated hardware for release data.

`storage_read` is the NVMe/O_DIRECT gate for storage-read claims. It emits
`pread`, `io_uring_read`, `read_fixed`, and `iopoll_read_fixed` rows, labels them
as `live-kernel-sanity`, and skips with a clear stderr reason when the benchmark
file is not NVMe-backed. Pass `--path /mnt/nvme/conflux-storage-read.bin` to place
the file on the intended device, or `--allow-non-nvme` only for smoke runs whose
numbers are not performance evidence.

For a local storage-read evidence artifact, use:

```sh
STORAGE_READ_PATH=/mnt/nvme/conflux-storage-read.bin \
  STORAGE_READ_REPS=5 scripts/storage_read_evidence.sh
```

The wrapper records configure/build logs, repeated raw NDJSON, a manifest with
host/build/device metadata, and the exact matrix used for `depth_1_4k`,
`depth_8_16k`, `depth_32_64k`, and `depth_128_1m`. It rejects non-NVMe files by
default; set `STORAGE_READ_ALLOW_NON_NVME=1` only for smoke runs.

`kernel_state_synthetic` is the no-kernel state-transition floor for socket and
file rows that otherwise include fd-table/TCP/io_uring round trips. It covers
direct-slot lease/release, direct-slot close lifecycle, deferred-close queue,
generation advance/alive checks, stale-generation rejection, and pure
`CompletionTable::dispatch` depth slopes. Treat these rows as `micro/user-space`.

`db_protocol_synthetic` is the DB-independent PostgreSQL row decode/protocol
floor. It builds fake `PGresult` objects and PostgreSQL `DataRow` byte streams in
memory, then measures text libpq row decode, text wire scanning, and binary wire
scanning without a PostgreSQL server, TCP, query planner, or socket wakeups.

`tls_mem_bio` is the steady-state TLS encode/decode floor. It creates a warm
client/server OpenSSL session over memory BIOs and records client-to-server and
round-trip echo costs for 4 KiB, 64 KiB, and 1 MiB payloads, without sockets or
handshake-dominated loopback rows.

`send_zc` records threshold sweep configs (`threshold_4k`, `threshold_16k`,
`threshold_64k`) across plain and mapped response sizes, plus concurrent HTTP
load configs (`threshold_4k_load`, `threshold_16k_load`, `threshold_64k_load`)
that run 64 keep-alive clients for 2 seconds against 64 KiB and 1 MiB bodies.
Use emitted `zc_*` counters to reject thresholds that mostly copy, fall back, or
bypass TLS; do not use RPS alone as SEND_ZC evidence. The `_load` rows also
include `connections`, `duration_s`, `requests_per_sec`, and `errors`, so the
default threshold can be judged under pressure before changing
`Config::send_zc_threshold`.

For a DB-independent threshold artifact, use:

```sh
SEND_ZC_PRESET=perf-clang-libcxx SEND_ZC_REPS=5 \
  scripts/send_zc_threshold_evidence.sh
```

The wrapper writes configure/build logs, raw repeated NDJSON, a manifest, and a
summary JSON with off-vs-`zc_auto` median/best speedups, concurrent-load RPS
speedups, copied-notification rates, submit fallback rates, per-threshold
rollups, candidate-body classification, and SEND_ZC ring capability/enabled
telemetry when emitted by the benchmark. See
`benchmarks/notes/send_zc_threshold_evidence.md` for the decision shape.

For a non-loopback SEND_ZC candidate artifact, use a host address that is not
`127.0.0.0/8` or `0.0.0.0`:

```sh
SEND_ZC_NIC_HOST=192.0.2.10 SEND_ZC_PRESET=perf-clang-libcxx SEND_ZC_REPS=5 \
  scripts/send_zc_nic_evidence.sh
```

This records paired `nic/plain/*/{off,zc_auto}` and
`nic/mapped/*/{off,zc_auto}` rows with request rate plus server-side SEND_ZC
counters in the same rows. Treat it as `live-kernel/NIC-candidate` evidence:
keep route/NIC counter output with the artifact before using the result to make
public zero-copy throughput claims. For smoke only, set
`SEND_ZC_ALLOW_LOOPBACK=1`.

For worker queue contention profiling, configure the perf preset with
`-DCONFLUX_WORK_QUEUE_STATS=ON` before recording the queue benchmarks. The
benchmarks emit the standard `config`/`variant`/`iterations`/`total_ns`/
`ns_per_iter` fields, and append a `queue` object in raw NDJSON with enqueue,
local/inject queue, admission/local/steal lock-contention, steal, park, futex wake,
and queue discard counters. Queue-mode comparison rows also include a `fairness`
object; the `local_backlog_redistribution` profile uses it to report runner-thread
count and max-runner share for work-conservation checks. `scripts/bench_record.sh` preserves optional standard-parser
fields in `results.extra`, so these counters are queryable as `extra->'queue'`
for non-summary rows while remaining available verbatim in raw artifacts.
`workpool_enqueue_dequeue` keeps its historical variant names for baseline
comparisons. `workpool_queue_mode_compare` is the separate mode-comparison
benchmark; it emits every queue-profile variant for both WorkPool queue modes,
using variant names like `stealing/external_burst` and
`no_stealing/external_burst`, so the mutex/job-deque/stealing path and the atomic
ring/no_stealing path can be compared within the same thread config. Normal
perf presets leave this option off so instrumentation does not contaminate
default history.

For a DB-independent evidence artifact, use the wrapper below. It enables queue
stats for the selected build preset, runs repeated `workpool_queue_mode_compare`
JSON reps, validates that each config has both queue modes for all five
queue-profile variants, and writes a summary JSON with no-stealing-vs-stealing
median deltas plus aggregate contention, fairness, and futex-wait rates per 1k
jobs for each mode-prefixed variant.

```sh
WORK_QUEUE_PRESET=perf-clang-libcxx \
WORK_QUEUE_THREADS=16 \
WORK_QUEUE_WORK=2048 \
WORK_QUEUE_REPS=5 \
  scripts/work_queue_contention_evidence.sh
```

To summarize an existing raw artifact without rebuilding, run:

```sh
python3 scripts/work_queue_contention_summary.py \
  /tmp/conflux/work-queue-evidence/<stamp>/workpool_queue_mode_compare.raw.ndjson \
  --output /tmp/conflux/work-queue-evidence/<stamp>/workpool_queue_mode_compare.summary.json
```

## Comparing runs

Two views are provided for comparing recorded runs.

### `bench_compare_summary` — highest differentiators

Returns one row per (benchmark, config, variant) pair with summary stats for both
runs. Best for finding what changed the most.

```sql
SELECT benchmark, config_name, variant, a_med_ns, b_med_ns, pct_change
FROM bench_compare_summary
WHERE run_a = 101 AND run_b = 102
ORDER BY ABS(pct_change) DESC;
```

Columns: `a_med_ns`, `b_med_ns` (medians), `a_mad`/`b_mad` (median absolute
deviations), `pct_change` (positive = run_b slower), `reps`.

### `bench_raw` — per-rep detail with summary stats

Pairs reps from `run_a` and `run_b` (matched by rank within variant), with
pre-computed medians, MAD, and `pct_change`. Use `SELECT *` like the summary
view.

```sql
SELECT *
FROM bench_raw
WHERE run_a = 101 AND run_b = 102
  AND benchmark = 'task_chain_composition' AND config_name = 'steps_1'
ORDER BY variant, a_ns;
```

Columns: `a_ns`/`b_ns` (individual rep), `a_med_ns`/`b_med_ns`,
`a_mad`/`b_mad`, `pct_change` (median-based). Useful for spotting bimodal
distributions or outlier reps that inflate the summary median.

## Regression budgets

`scripts/bench_db_migrate.sql` seeds explicit per-benchmark budget rules in
`bench_budgets`. Override a budget by inserting a more specific
`(benchmark, config_name, variant)` row; `*` is the wildcard. The merge gate uses
`bench_budget_eval`, which classifies each matched summary row as:

- `fail` — candidate median is slower than the configured regression budget.
- `noisy` — sample count or median absolute deviation is outside the rule.
- `unbudgeted` — no enabled rule matched the benchmark/config/variant.
- `pass` / `improved` — within budget.

Check a same-machine baseline/candidate pair:

```sh
scripts/bench_check_budget.py \
  --baseline-run-id 101 \
  --candidate-run-id 102 \
  --json-out /tmp/conflux-bench/budget-101-102.json
```

The checker exits non-zero for regressions, noisy rows that should be rerun,
unbudgeted rows, missing summaries, or machine mismatches. Its text report prints
both run artifact directories so CI logs point back to the recorded manifest,
bench-info, raw NDJSON, cache, and build logs.

## Comparing pre-built binaries

Use this for branch-vs-branch or baseline-vs-candidate binaries built in
separate trees. The launcher scans `/tmp` for `release-*` and `perf-*` benchmark
binaries that expose the requested logical benchmark.

```sh
BENCH_REPS=7 scripts/compare_bins_by_bench.sh --yes http_server
BENCH_REPS=7 scripts/compare_bins_by_bench.sh --yes file_copy_coro
BENCH_REPS=7 scripts/compare_bins_by_bench.sh --yes work
```

For fixed input sizes, reuse a previous run's summary iteration counts:

```sh
BENCH_REPS=7 scripts/compare_bins_by_bench.sh --yes \
  --baseline-run-id 101 http_server
```

## JSON corpus fixtures

`conflux_json_bench` also consumes source-relative fixtures under
`benchmarks/corpus/`. The root files are real-world parse/dump corpora;
`route_payloads/` covers application-shaped request/response JSON; `edge/`
contains valid adversarial inputs; and `malformed/` contains strict-JSON
rejection inputs. Keep these fixtures deterministic so benchmark history remains
comparable.

`conflux_json_bench` includes manual `JsonMembers<T>` DOM/direct decode and
write rows. `conflux_json_reflect_bench` is built only with
`CONFLUX_JSON_REFLECT=ON` and reports the matching P2996 reflected rows.

## Benchmark groups

Current groups:

- `micro/*`: small hot-path operations.
- `flow/*`: full in-process request flows through the public router/middleware API.
- `http_app_path`: user-space HTTP app-path scenarios: parse request bytes,
  construct request views, dispatch routes/middleware, build responses, and
  serialize headers/body without live socket/kernel round trips.
- `http_parser`: user-space HTTP/1 request-parser scenarios, including
  adversarial large/many/malformed/incomplete header cases that avoid live
  socket/kernel round trips.
- `http_adversarial`: user-space parser/security load scenarios with structured
  reject reasons, consumed bytes, CPU/request, and allocation counters.
- `slow_consumer_backpressure`: live slow-client/socket pressure rows plus
  local bounded SSE and WorkPool queue-full policy rows.
- `http_server`, `http_server_concurrency`, `static_strategy_matrix`, `send_zc`,
  `tcp_increment`, and `socket_raw`: HTTP/socket/io_uring transport measurements.
  Short `http_server_concurrency` rows are smoke-quality
  `live-kernel-sanity`; 30s+ rows with 5s+ warmup are labeled
  `end-to-end-proof`.
- `static_strategy_matrix`: live-kernel-sanity static-file rows for hot mmap
  fallback, splice-capable streamed files, range requests, small-file cache
  hits, and cache churn. It does not drop the kernel page cache and does not
  prove TLS read+write; pair it with `storage_read` and a TLS static consumer
  before making public static-file throughput claims.
- `file_copy_coro`: file/runtime measurements, including cached/no-fsync rows and `copy_odirect` when supported.
- `kernel_state_synthetic`: no-kernel fd-slot/generation/deferred-close/CQE-dispatch state-transition baselines.
- `db_protocol_synthetic`: DB-independent PostgreSQL result/protocol decode rows.
- `tls_mem_bio`: no-socket steady-state TLS encode/decode rows.
- `uring_completion` and `synthetic_cqe_coro`: no-kernel io_uring-adjacent
  microbenchmarks for CompletionTable dispatch, coroutine completion plumbing,
  synthetic file-read/socket-send loops, and cancel-before/after-completion cost.
- `work`, `task_*`, `workpool_*`, and `join_all_N`: worker/runtime measurements.

`conflux_send_zc_bench` emits per-variant SEND_ZC counter fields in its NDJSON
(`zc_attempts`, `zc_plain_attempts`, `zc_mapped_attempts`, copied-notification
counts, submit-fallback counts, pending notifications, and TLS-bypass counts)
plus `zc_capable_rings` and `zc_enabled_rings`. Its `--concurrent` mode adds
duration-based keep-alive load rows with request rate and error counters. Raw
recorder artifacts therefore preserve enough data to decide whether mapped-file
bodies should keep using SEND_ZC, whether TLS paths should remain explicit
regular-send bypasses, whether the host actually exposes/enables SEND_ZC, and
whether the default threshold should stay at 16 KiB or move to the 4 KiB/64 KiB
alternatives.

Network or io_uring transport benchmarks should remain separate cases rather
than being mixed into the in-process logic suite.

## External HTTP comparison harness

`scripts/http_external_compare.py` is a host-run evidence harness for public HTTP
claims against external servers such as Drogon, uWebSockets, cpp-httplib, or a
minimal Beast server. It keeps external applications out of this source tree: the
operator supplies commands and base URLs in a JSON spec, and the harness drives
every target with the same `wrk --latency` scenario matrix.

Start from the editable template:

```sh
cp benchmarks/external/http_compare.template.json /tmp/conflux-http-compare.json
$EDITOR /tmp/conflux-http-compare.json
HTTP_EXTERNAL_PIN_CPUS=0-7 \
  scripts/http_external_compare.py /tmp/conflux-http-compare.json --shuffle
```

The default scenario set matches the kernel-round-trip review: hello/plaintext,
JSON response, route param, middleware chain, POST echo 4 KiB/64 KiB, static
64 KiB/1 MiB, connect-close, and keepalive 32/256/1k. The artifact directory
contains `manifest.json`, per-target server logs, `raw.ndjson` with all raw
repetitions, and `summary.json` with the credibility rule: Conflux uses
worst-of-N request rate, external targets use best-of-N request rate. Do not
publish the summary without the raw rows, command lines, target commits/releases,
compiler flags, CPU/kernel/governor metadata, and pinned CPU settings.
