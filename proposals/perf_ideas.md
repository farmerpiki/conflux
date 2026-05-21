# Performance Ideas Checklist

Status: open inventory
Branch: `perf/evidence-inventory`
State note: this is the current performance inventory, not a single
implementation proposal. Use `todo/proposal_state.md` before selecting work.

This file intentionally avoids a detailed "implemented feature" inventory.
Those claims go stale quickly as source files move. Use code, tests, benchmark
notes, and `todo/proposal_state.md` as the authority for landed work.

## Open / Evidence-Gated

- [ ] `http/send-threshold-bench`: keep `Config::send_zc_threshold` unchanged
  until same-host artifacts show a stable win and SEND_ZC counters rule out
  copied notifications, submit fallback, or TLS bypass as the explanation.
  Next action is benchmark/path coverage, including a true mmap-response SEND_ZC
  run if send-path work continues.
- [ ] `uring/iopoll-static-evidence`: benchmark whether HTTP static serving is
  storage-read-bound before wiring storage-only IOPOLL into the HTTP/static path.
  Adoption is evidence-gated.
- [ ] `http/ring-layout-c2c-verify`: verify `Ring` hot/cold field grouping with
  `perf c2c` under load before adding padding or layout churn.
- [ ] `worker/queue-contention-followup`: only revisit local deque, steal-path,
  or `admission_mtx_` locking if contention counters under real HTTP load show a
  bottleneck.
- [ ] `perf/evidence-inventory`: keep this checklist current as benchmark
  evidence lands; do not turn it into a direct implementation branch.

## Deferred

- [ ] `uring/recv-zc`: implement `IORING_OP_RECV_ZC` only after target kernel
  support is stable enough for a narrow, runtime-probed branch. The current
  `RecvPayload` boundary is the intended future hook.
- [ ] `worker/p2300-prototype`: prototype a P2300/io_uring scheduler behind an
  experimental target only after the current runtime/API surface settles.
- [ ] Broad memory-order weakening in `WorkPool`: prove correctness before
  changing the current `seq_cst` wake/park fence pair.

## Measurement Rules

- [ ] Run benchmarks from release/perf presets, not debug or sanitizer builds.
- [ ] Capture same-machine baseline and candidate artifacts before accepting a
  perf claim.
- [ ] Use `benchmarks/tcp_increment_coro_bench` for socket/file coroutine
  latency and throughput, including `fr/*`, `str/*`, and `str/parallel_4`
  variants.
- [ ] Use `benchmarks/file_copy_coro_bench` for registered-buffer and file-copy
  path evidence.
- [ ] Use `conflux_send_zc_bench` plus `scripts/send_zc_threshold_evidence.sh`
  for SEND_ZC threshold evidence.
- [ ] Use `perf stat -e cache-misses,LLC-load-misses,dTLB-load-misses,cs` under
  realistic server load for low-level changes.
- [ ] Use `perf c2c` before changing `Ring` padding or hot/cold layout.
- [ ] Inspect `/proc/self/fdinfo/<ring_fd>` counters such as `sq_dropped`,
  `cq_overflow`, and `sq_busy` when tuning ring depth or batch behavior.
- [ ] Never claim a gain without numbers from the same hardware under realistic
  HTTP load.
