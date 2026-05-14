# Worker queue contention profile slice

Branch: `worker/queue-contention-profile`

This slice adds an opt-in counter surface for profiling `WorkPool` contention
without changing scheduling semantics.

## Instrumented points

- producer admission: enqueue attempts, stopped/full rejections, admission lock acquisitions
- local queue path: push/full, pop attempts/hits
- inject queue path: push/full, pop attempts/hits
- steal path: steal rounds, victim mutex checks, successful steals
- wake/park path: `wake_one`, `wake_all`, futex wakes, park attempts, recheck skips, futex waits
- execution: jobs run

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

## No-change decision for locks in this slice

No lock removal is included here. `admission_mtx_` is still the correctness gate
that prevents `drain_and_stop()` from observing `pending_ == 0` while a racing
enqueue has passed the stopped check but has not yet incremented `pending_`.
Removing it safely needs either an admission-state protocol or a producer epoch,
and should be justified by measured contention first.

The local deque mutexes also remain unchanged. They are required for local push,
owner pop, and cross-worker steal. The new counters distinguish local queue hits
from steal victim checks so a later patch can decide whether to change deque
layout, batching, or steal policy instead of speculatively replacing locks.
