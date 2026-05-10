# Migration Gates

Process gate for server migration steps. Run before merging any migration PR.
Not in CI — bench DB is local only.

## How to run

Build base and candidate binaries into separate paths, then compare:

```sh
# Build base (on main or pre-migration commit):
cmake --build /tmp/conflux/debug-clang-libcxx --target conflux_http_server_bench conflux_http_server_concurrency_bench

cp /tmp/conflux/debug-clang-libcxx/benchmarks/conflux_http_server_bench \
   /tmp/conflux-base/benchmarks/conflux_http_server_bench
cp /tmp/conflux/debug-clang-libcxx/benchmarks/conflux_http_server_concurrency_bench \
   /tmp/conflux-base/benchmarks/conflux_http_server_concurrency_bench

# Build candidate (on migration branch):
cmake --build /tmp/conflux/debug-clang-libcxx --target conflux_http_server_bench conflux_http_server_concurrency_bench

cp /tmp/conflux/debug-clang-libcxx/benchmarks/conflux_http_server_bench \
   /tmp/conflux-cand/benchmarks/conflux_http_server_bench
cp /tmp/conflux/debug-clang-libcxx/benchmarks/conflux_http_server_concurrency_bench \
   /tmp/conflux-cand/benchmarks/conflux_http_server_concurrency_bench
```

Run each binary comparison separately:

```sh
BENCH_REPS=7 scripts/bench_record.sh --compare-bins \
  base:/tmp/conflux-base/benchmarks/conflux_http_server_bench \
  cand:/tmp/conflux-cand/benchmarks/conflux_http_server_bench

BENCH_REPS=7 scripts/bench_record.sh --compare-bins \
  base:/tmp/conflux-base/benchmarks/conflux_http_server_concurrency_bench \
  cand:/tmp/conflux-cand/benchmarks/conflux_http_server_concurrency_bench
```

The script prints the run IDs for each pair. Use those IDs in the query below.

## Threshold check

Replace `:base_run_id` / `:cand_run_id` with the run IDs printed by `--compare-bins`:

```sql
SELECT
  variant,
  a_med_ns,
  b_med_ns,
  pct_change,
  reps
FROM bench_compare_summary
WHERE run_a = :'base_run_id'
  AND run_b = :'cand_run_id'
  AND pct_change > 5.0
ORDER BY pct_change DESC;
```

**Gate rule: merge is blocked if this query returns any row**, unless the regression is
explicitly accepted with a note in the PR description explaining why.

## Accepting a regression

If a variant regresses > 5% but the tradeoff is justified, document it:

```
Accepted regression: <variant> +<N>% — reason: <why this is acceptable>
```

Add this to the PR description before merging.
