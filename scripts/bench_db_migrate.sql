-- bench_db_migrate.sql — idempotent schema for conflux_bench.
-- Apply once before first bench_record.sh run:
--   psql postgres://postgres@localhost/conflux_bench -f scripts/bench_db_migrate.sql
--
-- Additive only: never drops columns, never renames. Old query patterns keep working.

-- ---------------------------------------------------------------------------
-- runs
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS runs (
    id            serial       PRIMARY KEY,
    name          text         NOT NULL,
    commit_sha    text         NOT NULL,
    branch        text         NOT NULL,
    dirty         boolean      NOT NULL DEFAULT false,
    host          text         NOT NULL,
    build_preset  text         NOT NULL,
    compiler      text         NOT NULL,
    benchmark     text         NOT NULL,
    config_name   text         NOT NULL DEFAULT '',
    config_extra  jsonb        NOT NULL DEFAULT '{}',
    created_at    timestamptz  NOT NULL DEFAULT now()
);

ALTER TABLE runs ADD COLUMN IF NOT EXISTS machine_id    text;
ALTER TABLE runs ADD COLUMN IF NOT EXISTS metadata      jsonb  NOT NULL DEFAULT '{}';
ALTER TABLE runs ADD COLUMN IF NOT EXISTS waiver_reason text;

-- Backfill machine_id for rows that predate the column.
UPDATE runs SET machine_id = 'm0' WHERE machine_id IS NULL;

-- ---------------------------------------------------------------------------
-- results
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS results (
    id          serial   PRIMARY KEY,
    run_id      integer  NOT NULL REFERENCES runs(id),
    benchmark   text     NOT NULL,
    variant     text     NOT NULL,
    iterations  bigint   NOT NULL,
    total_ns    bigint   NOT NULL,
    ns_per_iter double precision NOT NULL,
    extra       jsonb    NOT NULL DEFAULT '{}'
);

ALTER TABLE results ADD COLUMN IF NOT EXISTS metric       text             NOT NULL DEFAULT 'ns_per_iter';
ALTER TABLE results ADD COLUMN IF NOT EXISTS value        double precision;
ALTER TABLE results ADD COLUMN IF NOT EXISTS unit         text;
ALTER TABLE results ADD COLUMN IF NOT EXISTS sample_count bigint;
ALTER TABLE results ADD COLUMN IF NOT EXISTS median       double precision;
ALTER TABLE results ADD COLUMN IF NOT EXISTS mad          double precision;
ALTER TABLE results ADD COLUMN IF NOT EXISTS p50          double precision;
ALTER TABLE results ADD COLUMN IF NOT EXISTS p99          double precision;
ALTER TABLE results ADD COLUMN IF NOT EXISTS best         double precision;
ALTER TABLE results ADD COLUMN IF NOT EXISTS p10          double precision;

-- ---------------------------------------------------------------------------
-- build_facts — static per-run facts (sizeof, ABI hashes, fallback alloc pct, …)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS build_facts (
    id         serial   PRIMARY KEY,
    run_id     integer  NOT NULL REFERENCES runs(id),
    fact       text     NOT NULL,
    value      double precision,
    value_text text,
    unit       text,
    extra      jsonb    NOT NULL DEFAULT '{}'
);

-- ---------------------------------------------------------------------------
-- Indexes
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS runs_commit_bench   ON runs    (commit_sha, benchmark);
CREATE INDEX IF NOT EXISTS results_run_variant ON results (run_id, variant);
CREATE INDEX IF NOT EXISTS build_facts_run     ON build_facts (run_id, fact);

-- ---------------------------------------------------------------------------
-- Views used by benchmarks/README.md and migration gate notes
-- ---------------------------------------------------------------------------
CREATE OR REPLACE VIEW bench_compare_summary AS
SELECT
    a.run_id AS run_a,
    b.run_id AS run_b,
    ra.name  AS name_a,
    rb.name  AS name_b,
    a.benchmark,
    ra.config_name,
    a.variant,
    a.ns_per_iter AS a_med_ns,
    b.ns_per_iter AS b_med_ns,
    a.mad AS a_mad,
    b.mad AS b_mad,
    CASE
        WHEN a.ns_per_iter = 0 THEN NULL
        ELSE ((b.ns_per_iter - a.ns_per_iter) / a.ns_per_iter) * 100.0
    END AS pct_change,
    LEAST(COALESCE(a.sample_count, 0), COALESCE(b.sample_count, 0)) AS reps
FROM results a
JOIN results b
  ON b.benchmark = a.benchmark
 AND b.variant = a.variant
 AND b.run_id <> a.run_id
JOIN runs ra ON ra.id = a.run_id
JOIN runs rb ON rb.id = b.run_id
 AND rb.config_name = ra.config_name
WHERE a.extra->>'kind' = 'summary'
  AND b.extra->>'kind' = 'summary';

CREATE OR REPLACE VIEW bench_raw AS
WITH raw AS (
    SELECT
        r.run_id,
        runs.name,
        runs.config_name,
        r.benchmark,
        r.variant,
        r.iterations,
        r.total_ns,
        r.ns_per_iter,
        r.extra,
        ROW_NUMBER() OVER (
            PARTITION BY r.run_id, r.benchmark, runs.config_name, r.variant
            ORDER BY r.ns_per_iter, r.id
        ) AS rep_rank
    FROM results r
    JOIN runs ON runs.id = r.run_id
    WHERE COALESCE(r.extra->>'kind', '') <> 'summary'
), summary AS (
    SELECT
        r.run_id,
        runs.config_name,
        r.benchmark,
        r.variant,
        r.ns_per_iter AS med_ns,
        r.mad,
        r.p50,
        r.p99,
        r.best,
        r.p10
    FROM results r
    JOIN runs ON runs.id = r.run_id
    WHERE r.extra->>'kind' = 'summary'
)
SELECT
    a.run_id AS run_a,
    b.run_id AS run_b,
    a.name AS name_a,
    b.name AS name_b,
    a.benchmark,
    a.config_name,
    a.variant,
    a.rep_rank,
    a.ns_per_iter AS a_ns,
    b.ns_per_iter AS b_ns,
    sa.med_ns AS a_med_ns,
    sb.med_ns AS b_med_ns,
    sa.mad AS a_mad,
    sb.mad AS b_mad,
    sa.p50 AS a_p50,
    sb.p50 AS b_p50,
    sa.p99 AS a_p99,
    sb.p99 AS b_p99,
    sa.best AS a_best,
    sb.best AS b_best,
    sa.p10 AS a_p10,
    sb.p10 AS b_p10,
    CASE
        WHEN sa.med_ns = 0 THEN NULL
        ELSE ((sb.med_ns - sa.med_ns) / sa.med_ns) * 100.0
    END AS pct_change
FROM raw a
JOIN raw b
  ON b.benchmark = a.benchmark
 AND b.config_name = a.config_name
 AND b.variant = a.variant
 AND b.rep_rank = a.rep_rank
 AND b.run_id <> a.run_id
LEFT JOIN summary sa
  ON sa.run_id = a.run_id
 AND sa.benchmark = a.benchmark
 AND sa.config_name = a.config_name
 AND sa.variant = a.variant
LEFT JOIN summary sb
  ON sb.run_id = b.run_id
 AND sb.benchmark = b.benchmark
 AND sb.config_name = b.config_name
 AND sb.variant = b.variant;
