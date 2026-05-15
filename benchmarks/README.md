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
cmake --build --preset perf-clang-libcxx --target conflux_record_benches -j1
```

or:

```sh
cmake --preset perf-gcc-stdcxx
cmake --build --preset perf-gcc-stdcxx --target conflux_record_benches -j1
```

Focused debug builds are still available when you need instrumentation or a
debugger, but do not use sanitizer/debug presets for performance conclusions.
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

`--bench-info` JSON shape:

```json
{
  "name":    "logical_bench_name",
  "parser":  "standard|tcp_parallel|file_copy",
  "configs": [
    { "name": "cfg", "extra": {}, "args": ["--iterations", "N"], "reps": 2 }
  ]
}
```

Parsers:

- `standard` — NDJSON `variant,iterations,total_ns,ns_per_iter`; recorded via
  `record_with_reps`.
- `tcp_parallel` — custom parser; configs sourced from `benchmarks/configs/*.json`.
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

Focused component commands:

```sh
# HTTP/server path
ONLY_BENCH=http_server BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-server-local
ONLY_BENCH=http_server_concurrency BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh http-server-concurrency-local
ONLY_BENCH=send_zc BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh send-zc-threshold-local

# File/runtime path
ONLY_BENCH=file_copy_coro BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh file-copy-local

# Worker/runtime path
ONLY_BENCH=work BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh work-local
ONLY_BENCH=task_chain_composition BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh task-chain-local
ONLY_BENCH=workpool_enqueue_dequeue BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh workpool-local
```

`send_zc` records threshold sweep configs (`threshold_4k`, `threshold_16k`,
`threshold_64k`) across plain and mapped response sizes, plus concurrent HTTP
load configs (`threshold_4k_load`, `threshold_16k_load`, `threshold_64k_load`)
that run 64 keep-alive clients for 2 seconds against 64 KiB and 1 MiB bodies.
Use emitted `zc_*` counters to reject thresholds that mostly copy, fall back, or
bypass TLS; do not use RPS alone as SEND_ZC evidence. The `_load` rows also
include `connections`, `duration_s`, `requests_per_sec`, and `errors`, so the
default threshold can be judged under pressure before changing
`Config::send_zc_threshold`.

For worker queue contention profiling, configure the perf preset with
`-DCONFLUX_WORK_QUEUE_STATS=ON` before recording `workpool_enqueue_dequeue`. The
benchmark still emits the standard `config`/`variant`/`iterations`/`total_ns`/
`ns_per_iter` fields, and appends a `queue` object in raw NDJSON with enqueue,
local/inject queue, admission/local/steal lock-contention, steal, park, and
futex wake counters. `scripts/bench_record.sh` preserves optional standard-parser
fields in `results.extra`, so these counters are queryable as `extra->'queue'`
for non-summary rows while remaining available verbatim in raw artifacts. The
benchmark includes the original per-task-join variants plus `external_burst` for
admission/inject pressure and `local_fanout` for local-deque/steal pressure.
Normal perf presets leave this option off so instrumentation does not
contaminate default history.

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

## Benchmark groups

Current groups:

- `micro/*`: small hot-path operations.
- `flow/*`: full in-process request flows through the public router/middleware API.
- `http_server`, `http_server_concurrency`, `send_zc`, `tcp_increment`, and
  `socket_raw`: HTTP/socket/io_uring transport measurements.
- `file_copy_coro`: file/runtime measurements.
- `work`, `task_*`, `workpool_*`, and `join_all_N`: worker/runtime measurements.

`conflux_send_zc_bench` emits per-variant SEND_ZC counter fields in its NDJSON
(`zc_attempts`, `zc_plain_attempts`, `zc_mapped_attempts`, copied-notification
counts, submit-fallback counts, and TLS-bypass counts). Its `--concurrent` mode
adds duration-based keep-alive load rows with request rate and error counters.
Raw recorder artifacts therefore preserve enough data to decide whether
mapped-file bodies should keep using SEND_ZC, whether TLS paths should remain
explicit regular-send bypasses, and whether the default threshold should stay at
16 KiB or move to the 4 KiB/64 KiB alternatives.

Network or io_uring transport benchmarks should remain separate cases rather
than being mixed into the in-process logic suite.
