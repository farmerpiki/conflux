# Migration Gates

Process gate for server migration steps. Run before merging any migration PR.
Not in CI — bench DB is local only.

Known-good baseline for comparisons in this worktree: `c3453a3`.
Use it as the reference commit for perf and migration checks unless a newer
baseline is called out explicitly.

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

The script prints integer run IDs for each label:

```
  base → run_id=123
  cand → run_id=124
```

Use those IDs in the query below.

## Threshold check

`bench_compare_summary.run_a` / `run_b` are the integer `id` values from the `runs` table
(not the `name` column — all `--compare-bins` runs share name `"compare-bins"`).
Substitute the printed integers directly (no quotes):

```sql
-- Replace 123 / 124 with the run IDs printed by --compare-bins.
SELECT
  variant,
  a_med_ns,
  b_med_ns,
  pct_change,
  reps
FROM bench_compare_summary
WHERE run_a = 123
  AND run_b = 124
  AND pct_change > 5.0
ORDER BY pct_change DESC;
```

If unsure of the column types, verify first: `\d bench_compare_summary`

**Gate rule: merge is blocked if this query returns any row**, unless the regression is
explicitly accepted with a note in the PR description explaining why.

## Accepting a regression

If a variant regresses > 5% but the tradeoff is justified, document it:

```
Accepted regression: <variant> +<N>% — reason: <why this is acceptable>
```

Add this to the PR description before merging.
