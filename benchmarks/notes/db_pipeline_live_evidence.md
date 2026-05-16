# DB pipeline live evidence lane

Branch: `db/pipeline-live-evidence`

Goal: close the remaining P7 validation gap with host-local PostgreSQL evidence,
without touching the DB runtime or `recv_bundle.e2e`-adjacent HTTP/socket paths.

## What changed for this lane

- `conflux_db_integration` discovered tests now carry the `db;integration` CTest
  label, so the live PostgreSQL slice can run without selecting unrelated tests.
- `scripts/bench_record.sh` now summarizes standard NDJSON rows that do not carry
  optional `min` / `p10` fields. This matters for `db_pipeline_bench`: it emits
  the standard `BenchStats` fields only, so previous recorded runs inserted raw
  rows but no `kind=summary` rows.
- `scripts/db_pipeline_live_evidence.sh` provides a no-benchmark-DB evidence path:
  it builds the DB integration target and `conflux_db_pipeline_bench`, runs the
  DB-labeled integration tests, validates the repeated raw pipeline benchmark rows,
  then writes a small summary JSON artifact.

## Run direct evidence without the benchmark DB

Use a disposable PostgreSQL database or schema. The benchmark creates a temporary
`conflux_pipeline_bench` table on its connection, but integration tests create and
mutate ordinary test tables, so keep this pointed at a test-only database.

```sh
PG_TEST_CONNINFO='host=localhost user=postgres dbname=conflux_test' \
PG_CONNINFO='host=localhost user=postgres dbname=conflux_bench' \
DB_PIPELINE_PRESET=release-gcc-stdcxx \
DB_PIPELINE_REPS=5 \
scripts/db_pipeline_live_evidence.sh
```

Artifacts are written under:

```text
/tmp/conflux/db-pipeline-evidence/<UTC-stamp>/
```

Expected files:

- `ctest-db-integration.log` — live DB integration result.
- `db_pipeline.raw.ndjson` — repeated `plain` and `pipeline` rows annotated with
  `rep`, so each launch can be paired in later analysis.
- `db_pipeline.summary.json` — median/best per variant plus paired per-rep
  speedups, `speedup_median`, and `speedup_best`. The script refuses to write
  this if the raw NDJSON is missing either `plain` or `pipeline` rows for any
  repetition.
- `manifest.json` — build dir, preset, commit/branch where available.

## Run recorded evidence through `conflux_bench`

Apply the schema once:

```sh
psql "$PGURI" -f scripts/bench_db_migrate.sql
```

Record only the pipeline benchmark:

```sh
PGURI='postgres://postgres@localhost/conflux_bench' \
PG_CONNINFO='host=localhost user=postgres dbname=conflux_bench' \
ONLY_BENCH=db_pipeline \
BENCH_PRESET=perf-gcc-stdcxx \
BENCH_REPS=5 \
scripts/bench_record.sh db-pipeline-wire-mode
```

The recorder now inserts `kind=summary` rows for `db_pipeline` even though its raw
rows do not include optional intra-benchmark distribution fields.

Useful query after recording:

```sql
WITH s AS (
  SELECT r.run_id, runs.build_preset, runs.config_name, r.variant,
         r.ns_per_iter, r.sample_count
  FROM results r
  JOIN runs ON runs.id = r.run_id
  WHERE r.benchmark = 'db_pipeline'
    AND r.extra->>'kind' = 'summary'
)
SELECT p.run_id,
       p.build_preset,
       p.config_name,
       plain.ns_per_iter AS plain_ns,
       p.ns_per_iter AS pipeline_ns,
       plain.ns_per_iter / p.ns_per_iter AS speedup,
       LEAST(plain.sample_count, p.sample_count) AS reps
FROM s p
JOIN s plain ON plain.run_id = p.run_id
            AND plain.config_name = p.config_name
            AND plain.variant = 'plain'
WHERE p.variant = 'pipeline'
ORDER BY p.run_id DESC;
```

## Evidence status in this environment

Not captured here: the available container has libpq headers/libraries but no
PostgreSQL server or `psql`/`pg_isready` client tools. I verified the script
syntax, raw-row validation, and JSON summarizer locally; live numbers must be
produced on a host with PostgreSQL reachable through `PG_TEST_CONNINFO` and
`PG_CONNINFO`.
