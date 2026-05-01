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
