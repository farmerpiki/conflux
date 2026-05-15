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

`workpool_enqueue_dequeue` now reports four variants per config:

- `single_thread` — one external producer, one worker, one blocking join per job
- `contended` — N external producers, N workers, one blocking join per job
- `external_burst` — N external producers enqueue a synchronized burst of counted jobs; stresses admission/inject without per-job join throttling
- `local_fanout` — one worker enqueues a fanout batch onto its local deque; stresses local deque locking and steal-victim probing

## Profiling command

```sh
cmake --preset perf-clang-libcxx -DCONFLUX_WORK_QUEUE_STATS=ON
cmake --build --preset perf-clang-libcxx --target conflux_workpool_enqueue_dequeue_bench -j1
/tmp/conflux/perf-clang-libcxx/benchmarks/conflux_workpool_enqueue_dequeue_bench \
  --threads 16 --iterations 5000 --warmup 500 --json
```

For recorded runs:

```sh
ONLY_BENCH=workpool_enqueue_dequeue BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh workpool-queue-profile
```

## No-change decision for locks in these slices

No lock removal is included here. `admission_mtx_` is still the correctness gate
that prevents `drain_and_stop()` from observing `pending_ == 0` while a racing
enqueue has passed the stopped check but has not yet incremented `pending_`.
Removing it safely needs either an admission-state protocol or a producer epoch,
and should be justified by measured contention first.

The local deque mutexes also remain unchanged. They are required for local push,
owner pop, and cross-worker steal. The follow-up counters distinguish actual
mutex contention from ordinary queue activity, so later patches can decide
between batching, steal-policy changes, per-worker queue layout changes, or no
scheduler change at all.
