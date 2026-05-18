# Worker queue contention profile/follow-up slice

Branches: `worker/queue-contention-profile`, `worker/queue-contention-followup`

These slices add an opt-in counter surface for profiling `WorkPool` contention
without changing scheduling semantics.

## Instrumented points

- producer admission: enqueue attempts, stopped/full rejections, admission lock acquisitions/contentions
- local queue path: local mutex acquisitions/contentions, push/full, pop attempts/hits
- inject queue path: push/full, pop attempts/hits
- steal path: steal rounds, victim mutex checks, victim mutex acquisitions/contentions, successful steals
- wake/park path: `wake_one`, `wake_all`, futex wakes, park attempts, recheck skips, futex waits
- execution: jobs run

The `*_lock_contentions` counters are intentionally cheap probes: with
`CONFLUX_WORK_QUEUE_STATS=ON`, each profiled mutex path first attempts a single
`try_lock()`. A miss increments the corresponding contention counter and then
falls back to normal blocking `lock()`. With queue stats disabled, the paths use
plain blocking mutex acquisition and avoid the profiling `try_lock()` probe.

## Benchmark variants

`workpool_queue_mode_compare` reports four queue-profile variants per config and per queue mode:

- `single_thread` — one external producer, one worker, one blocking join per job
- `contended` — N external producers, N workers, one blocking join per job
- `external_burst` — N external producers enqueue a synchronized burst of counted jobs; stresses admission/inject without per-job join throttling
- `local_fanout` — one worker enqueues a fanout batch onto its local deque; stresses local deque locking and steal-victim probing

## Profiling command

Use the evidence wrapper for host-local measurements that do not need the
benchmark database:

```sh
WORK_QUEUE_PRESET=perf-clang-libcxx \
WORK_QUEUE_THREADS=16 \
WORK_QUEUE_ITERATIONS=5000 \
WORK_QUEUE_WARMUP=500 \
WORK_QUEUE_REPS=5 \
  scripts/work_queue_contention_evidence.sh
```

Artifacts are written under:

```text
/tmp/conflux/work-queue-evidence/<UTC-stamp>/
```

Expected files:

- `configure.log` and `build.log` — queue-stats build evidence.
- `workpool_queue_mode_compare.raw.ndjson` — repeated raw benchmark rows.
- `workpool_queue_mode_compare.summary.json` — per-config/variant timing medians,
  aggregate queue counters, and admission/local/steal/futex rates per 1k jobs.
- `manifest.json` — build dir, preset, thread/rep counts, commit/branch where
  available.

Manual equivalent:

```sh
cmake --preset perf-clang-libcxx -DCONFLUX_WORK_QUEUE_STATS=ON
cmake --build --preset perf-clang-libcxx --target conflux_workpool_queue_mode_compare_bench -j1
/tmp/conflux/perf-clang-libcxx/benchmarks/conflux_workpool_queue_mode_compare_bench \
  --threads 16 --iterations 5000 --warmup 500 --json
```

For recorded runs:

```sh
ONLY_BENCH=workpool_queue_mode_compare BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh workpool-queue-profile
```

The recorder keeps optional fields from standard-parser NDJSON in raw
`results.extra`. Queue counters are therefore available both in the saved raw
artifact and in SQL for non-summary rows:

```sql
SELECT variant,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY ns_per_iter) AS med_ns,
       sum((extra->'queue'->>'admission_lock_contentions')::bigint) AS admission_contentions,
       sum((extra->'queue'->>'local_lock_contentions')::bigint) AS local_contentions,
       sum((extra->'queue'->>'steal_lock_contentions')::bigint) AS steal_contentions,
       sum((extra->'queue'->>'futex_waits')::bigint) AS futex_waits
FROM results
WHERE run_id = :run_id
  AND benchmark = 'workpool_queue_mode_compare'
  AND COALESCE(extra->>'kind', '') <> 'summary'
GROUP BY variant
ORDER BY variant;
```

## Follow-up decision

The first contention evidence showed that external-producer profiles paid
steal-victim scans even when no local jobs existed to steal. The follow-up keeps
default `stealing` semantics but gates victim scans behind a global count of
worker-local queued jobs. External-only workloads avoid the steal-lock path;
local-fanout workloads still expose their backlog to victim workers.

A post-gate rerun on `perf-clang-libcxx`, 8 workers, 5 reps, 5000 measured
iterations showed `no_stealing` ahead on every current microprofile:

| profile | stealing median ns/op | no_stealing median ns/op | no_stealing speedup |
| --- | ---: | ---: | ---: |
| `single_thread` | 9618.71 | 5435.55 | 1.77x |
| `contended` | 788.76 | 403.85 | 1.95x |
| `external_burst` | 670.42 | 347.92 | 1.93x |
| `local_fanout` | 1419.90 | 693.22 | 2.05x |

No default queue-mode change is made from this evidence alone. The current
`local_fanout` profile measures cheap queued-job throughput after one worker
produces a local batch; it does not measure latency/fairness under expensive
fanout, blocked workers, or uneven producer workers where victim stealing is the
work-conserving mechanism. Treat `no_stealing` as the throughput-preferred mode
for bounded independent offload work, and keep default `stealing` for general
executor semantics until those redistribution profiles exist and support a
default flip.

`admission_mtx_` remains the correctness gate for default stealing mode: it
prevents `drain_and_stop()` from observing `pending_ == 0` while a racing enqueue
has passed the stopped check but has not yet incremented `pending_`. Removing it
safely still needs either an admission-state protocol or a producer epoch, and
should be justified by measured contention first.
