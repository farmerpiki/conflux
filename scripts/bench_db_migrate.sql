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
    a.best AS a_best_ns,
    b.best AS b_best_ns,
    a.p10 AS a_p10_ns,
    b.p10 AS b_p10_ns,
    a.p50 AS a_p50_ns,
    b.p50 AS b_p50_ns,
    a.p99 AS a_p99_ns,
    b.p99 AS b_p99_ns,
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

-- ---------------------------------------------------------------------------
-- bench_budgets — merge-gate regression budgets.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS bench_budgets (
    id                 serial  PRIMARY KEY,
    benchmark          text    NOT NULL,
    config_name        text    NOT NULL DEFAULT '*',
    variant            text    NOT NULL DEFAULT '*',
    max_regression_pct double precision NOT NULL,
    min_samples        integer NOT NULL DEFAULT 5,
    max_mad_pct        double precision NOT NULL DEFAULT 15.0,
    enabled            boolean NOT NULL DEFAULT true,
    note               text    NOT NULL DEFAULT ''
);

CREATE UNIQUE INDEX IF NOT EXISTS bench_budgets_key
    ON bench_budgets (benchmark, config_name, variant);

INSERT INTO bench_budgets
    (benchmark, config_name, variant, max_regression_pct, min_samples, max_mad_pct, note)
VALUES
    ('crypto', '*', '*', 6.0, 5, 15.0, 'CPU-bound microbenchmark'),
    ('db_coro', '*', '*', 12.0, 5, 20.0, 'local PostgreSQL coroutine path'),
    ('db_params', '*', '*', 7.5, 5, 15.0, 'CPU-bound DB parameter marshalling'),
    ('db_pipeline', '*', '*', 12.0, 5, 20.0, 'local PostgreSQL pipeline path'),
    ('file_copy_coro', '*', '*', 15.0, 2, 25.0, 'filesystem/runtime benchmark'),
    ('http_server', '*', '*', 20.0, 1, 100.0, 'full server smoke has one expensive rep'),
    ('http_server_concurrency', '*', '*', 20.0, 1, 100.0, 'duration-based server load row'),
    ('join_all_N', '*', '*', 10.0, 5, 20.0, 'worker fan-in variants'),
    ('json', '*', '*', 6.0, 5, 15.0, 'CPU-bound parser/dom benchmark'),
    ('router', '*', '*', 5.0, 5, 12.0, 'hot-path route lookup'),
    ('send_zc', '*', '*', 20.0, 1, 100.0, 'transport threshold sweep has expensive rows'),
    ('socket_raw', '*', '*', 15.0, 5, 25.0, 'local socket/io_uring transport path'),
    ('task_cancellation', '*', '*', 7.5, 5, 15.0, 'worker cancellation microbenchmark'),
    ('task_chain_composition', '*', '*', 8.0, 5, 15.0, 'worker chain composition'),
    ('task_creation', '*', '*', 7.5, 5, 15.0, 'worker task creation microbenchmark'),
    ('tcp_increment', '*', '*', 15.0, 5, 25.0, 'local TCP coroutine transport path'),
    ('template', '*', '*', 5.0, 5, 12.0, 'CPU-bound template rendering'),
    ('tls_tcp_increment_coro', '*', '*', 20.0, 5, 30.0, 'TLS local TCP coroutine path'),
    ('work', '*', '*', 10.0, 5, 20.0, 'worker scheduler benchmark'),
    ('workpool_enqueue_dequeue', '*', '*', 12.0, 5, 25.0, 'worker queue benchmark'),
    ('workpool_queue_mode_compare', '*', '*', 12.0, 5, 25.0, 'worker queue mode comparison benchmark')
ON CONFLICT (benchmark, config_name, variant) DO NOTHING;

CREATE OR REPLACE VIEW bench_budget_eval AS
SELECT
    c.run_a,
    c.run_b,
    c.name_a,
    c.name_b,
    c.benchmark,
    c.config_name,
    c.variant,
    c.a_med_ns,
    c.b_med_ns,
    c.a_best_ns,
    c.b_best_ns,
    c.a_p10_ns,
    c.b_p10_ns,
    c.a_p50_ns,
    c.b_p50_ns,
    c.a_p99_ns,
    c.b_p99_ns,
    c.a_mad,
    c.b_mad,
    CASE
        WHEN c.a_med_ns = 0 OR c.a_mad IS NULL THEN NULL
        ELSE (c.a_mad / c.a_med_ns) * 100.0
    END AS a_mad_pct,
    CASE
        WHEN c.b_med_ns = 0 OR c.b_mad IS NULL THEN NULL
        ELSE (c.b_mad / c.b_med_ns) * 100.0
    END AS b_mad_pct,
    c.pct_change,
    c.reps,
    b.max_regression_pct,
    b.min_samples,
    b.max_mad_pct,
    CASE
        WHEN b.id IS NULL THEN 'unbudgeted'
        WHEN c.pct_change IS NULL THEN 'noisy'
        WHEN c.reps < b.min_samples THEN 'noisy'
        WHEN c.a_med_ns <> 0 AND c.a_mad IS NOT NULL
             AND (c.a_mad / c.a_med_ns) * 100.0 > b.max_mad_pct THEN 'noisy'
        WHEN c.b_med_ns <> 0 AND c.b_mad IS NOT NULL
             AND (c.b_mad / c.b_med_ns) * 100.0 > b.max_mad_pct THEN 'noisy'
        WHEN c.pct_change > b.max_regression_pct THEN 'fail'
        WHEN c.pct_change <= 0 THEN 'improved'
        ELSE 'pass'
    END AS status
FROM bench_compare_summary c
LEFT JOIN LATERAL (
    SELECT budget.*
    FROM bench_budgets budget
    WHERE budget.enabled
      AND budget.benchmark = c.benchmark
      AND (budget.config_name = c.config_name OR budget.config_name = '*')
      AND (budget.variant = c.variant OR budget.variant = '*')
    ORDER BY
      (budget.config_name = c.config_name) DESC,
      (budget.variant = c.variant) DESC
    LIMIT 1
) b ON true;
