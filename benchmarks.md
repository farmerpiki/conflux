# Benchmark playbook

## Machine state before any bench run

- CPU governor: `performance` (`cpupower frequency-set -g performance`)
- Swap off: `swapoff -a`
- No concurrent heavy processes (builds, tests, etc.)
- `isolcpus=N-M` in kernel cmdline recommended when using `BENCH_PIN_CPUS`
- **Do not run any commands, editors, or monitoring tools while a bench is executing.**
  A hung bench is diagnosed after the fact; a timeout (`--timeout` or the `timeout(1)`
  wrapper) is the only safe intervention.

## CI lane split

Correctness and performance lanes are intentionally separate:

- `scripts/run-sanitizer-matrix.sh` configures/builds/tests sanitizer and debug correctness presets. Benchmark targets stay disabled there.
- `scripts/run-perf-matrix.sh` configures/builds benchmark targets from `perf-*` presets only. Sanitizers and tests stay disabled there.
- `scripts/bench_record.sh` is the measured benchmark recorder for quiet hosts and DB-backed result capture.

## Standard record run

Builds each perf preset, runs all discovered bench binaries, records to DB.

```sh
PGURI=postgres://postgres@localhost/conflux_bench \
BENCH_REPS=10 \
scripts/bench_record.sh [run-name]
```

Env knobs:

| var | default | meaning |
|---|---|---|
| `BENCH_PRESET` | `perf-clang-libcxx perf-gcc-stdcxx` | space-separated CMake presets |
| `BENCH_REPS` | 5 | outer replications per variant |
| `BENCH_PIN_CPUS` | (none) | `taskset -c` mask, e.g. `0-3` |
| `KEEP_BUILD=1` | (off) | skip build-dir cleanup |
| `ONLY_BENCH` | (all) | logical bench name filter |
| `WAIVER_REASON` | (none) | free-form note saved to `runs.waiver_reason` |

Each bench binary must implement `--bench-info` (returns JSON descriptor) and `--json`
(NDJSON output). Standard NDJSON format:

```json
{"config":"","variant":"name","iterations":N,"total_ns":N,"ns_per_iter":X,"min":X,"p10":X,"mad":X}
```

`min`/`p10`/`mad` are per-invocation statistics across R internal micro-reps (default R=10).
The outer `BENCH_REPS` runs are then aggregated by the script into a summary row
(`extra->>'kind'='summary'`) with `best=MIN(min)`, `p10=PERCENTILE_CONT(0.5 of p10s)`,
`median`, `mad`, `p50`, `p99`.

## Thermal-fair cross-candidate comparison

Use when comparing candidates from **different build trees** (different branches/designs):

### Step 1 — pre-build each candidate

```sh
# from each worktree:
cmake --preset perf-clang-libcxx
cmake --build /tmp/<tree>/perf-clang-libcxx --target conflux_work_benchmarks -- -j$(nproc)
```

### Step 2 — interleaved compare

```sh
BENCH_REPS=10 \
PGURI=postgres://postgres@localhost/conflux_bench \
scripts/bench_record.sh --compare-bins \
  "baseline:/tmp/work/release-clang-libcxx/benchmarks/conflux_work_benchmarks" \
  "d1:/tmp/work-p5-d1/release-clang-libcxx-p5/benchmarks/conflux_work_benchmarks" \
  "d2:/tmp/work-p5-d2/release-clang-libcxx-p5/benchmarks/conflux_work_benchmarks"
```

**Do not touch the machine while this runs.**
The script does a load check and aborts if load exceeds `nproc × 1.5`.
Wrap with `timeout` if you need a hard deadline:

```sh
timeout 1800 scripts/bench_record.sh --compare-bins ...
```

### Protocol

- All candidates built before any measurement.
- BENCH_REPS rounds total. Each round runs all N candidates once in rotating order
  (round `k` starts at candidate `(k-1) % N`).
- Constant 2 s settle **before every execution** (same within-round and between-round —
  different settle times make order effects uninterpretable).
- Load check before every execution; aborts if overloaded.
- Each raw result row tagged `{"round":R,"position":P,"label":"..."}` in `extra` JSONB
  for post-hoc order-effect analysis.

### Checking for order effects

```sql
SELECT label, variant,
       (extra->>'position')::int AS pos,
       round(AVG(ns_per_iter)::numeric, 2) AS avg_ns,
       count(*) AS n
FROM results
WHERE run_id IN (<run_ids>)
  AND extra->>'round' IS NOT NULL
GROUP BY label, variant, pos
ORDER BY variant, label, pos;
```

Flat avg_ns across positions = no thermal gradient at chosen settle time.
Trend (pos 1 < pos 3) = throttling; increase settle or use `isolcpus`.

### Comparing candidates

```sql
SELECT runs.config_extra->>'label' AS label,
       r.variant,
       round(r.best::numeric, 2)   AS best,
       round(r.p10::numeric, 2)    AS p10,
       round(r.median::numeric, 2) AS med,
       round(r.mad::numeric, 2)    AS mad,
       round((r.mad/r.median*100)::numeric, 1) AS mad_pct
FROM results r
JOIN runs ON runs.id = r.run_id
WHERE r.run_id IN (<run_ids>)
  AND r.extra->>'kind' = 'summary'
ORDER BY r.variant, label;
```

Prefer `best` for allocation-dominated microbench (shows pool hot-path minimum).
Prefer `p10` or `median` for throughput workloads with scheduling variance.
`mad_pct > 5%` on a CPU-bound bench = environmental noise; re-run after fixing governor/isolation.

## Comparing same-tree presets (--compare)

For comparing two or more CMake presets built from the **same source tree**
(e.g. a feature flag on/off):

```sh
BENCH_REPS=10 scripts/bench_record.sh --compare \
  release-clang-libcxx \
  release-clang-libcxx-p5
```

Builds each preset, then runs the same rotating-interleave protocol as `--compare-bins`.

## Adding a new bench binary

1. Implement `--bench-info` and `--json` in the binary (see contract at top of `bench_record.sh`).
2. Add the target to `conflux_record_benches` in `benchmarks/CMakeLists.txt`.
3. Link `conflux_bench_release_opts` for `-ffunction-sections -fdata-sections
   -falign-functions=64 -Wl,--gc-sections` (release/relwithdebinfo only).
4. Use `Case::reps = 1` for cases that create threads or do I/O per iteration.

## DB schema (results table)

| column | meaning |
|---|---|
| `ns_per_iter` | median of the R micro-reps (summary row) |
| `best` | `MIN(min)` across all outer reps — pool/allocator hot path |
| `p10` | `PERCENTILE_CONT(0.5)` of per-run p10 values |
| `median` / `mad` | median and median absolute deviation across outer reps |
| `p50` / `p99` | direct percentiles of `ns_per_iter` across outer reps |
| `extra->>'kind'='summary'` | marks the aggregated row (one per variant per run) |
| `extra->>'round'` | round index (raw rows from `--compare[-bins]` only) |
| `extra->>'position'` | position within the round (1 = ran first) |
