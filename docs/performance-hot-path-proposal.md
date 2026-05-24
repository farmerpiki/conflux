# Performance Hot-Path Evidence and Implementation Plan

Status: accepted as a performance-roadmap document; implementation must be
split into narrow tickets with evidence gates.

This proposal responds to the performance-practices review that rated the
project at 8.4/10 and called out async I/O allocation, task control-block
ownership, JSON parser architecture, JSON arena over-reservation, CPU
specialization, and external proof readiness.

The main rule for this work: no optimization is accepted from intuition alone.
Every implementation step needs a baseline, a candidate result, and a clear
rollback path.

## Quality Assessment

The proposal is directionally strong: it names real repository hot paths, keeps
the first implementation slice narrow, and treats performance evidence as a gate
instead of a post-hoc justification.

The weaker areas are:

- Release/perf build roles were blurred. Release binaries must be the acceptance
  proof; perf binaries are for profiling, symbols, and corroboration.
- The async I/O section names the allocation shape, but needs a sharper
  measurement gate for `std::function` spill behavior and completion dispatch.
- The JSON arena section identifies over-reservation, but the real constraint is
  pointer stability for duplicate-key checking: current duplicate-key hash sets
  can hold `string_view`s into `string_arena`, so any fix must keep those keys
  stable or change the duplicate-key representation.
- The research recommends whole-program/profile-guided optimization before
  manual branch/inlining/SIMD work. The proposal needs an explicit PGO/toolchain
  investigation path.
- Synchronization and queue contention are not addressed, even though the repo
  already has WorkPool queue-stat evidence and a benchmark surface for it.
- Compiler-dependent changes need generated-code, binary-size, and build-time
  checks in addition to throughput.

## Goals

- Prove any claimed speedup with before/after binaries.
- Use Clang, GCC 15, and GCC 16 release binaries as the acceptance gate.
- Use perf-profile binaries for profiling, symbolized diagnostics, and
  corroboration, not as a substitute for release results.
- Prefer small, measurable hot-path changes over broad rewrites.
- Preserve public API semantics unless a separate API migration is approved.
- Capture allocation and tail-latency effects, not only median throughput.
- Track binary size and build-time effects for template-heavy, dispatch-heavy,
  SIMD, PGO, LTO, and inlining changes.

## Benchmark Discipline

Benchmarking is acceptance evidence, not the first implementation task. Start
with the smallest measurement or instrumentation surface that can expose the
target cost, then implement one narrow slice, then run the relevant release
compare-bins when the host is idle enough for useful results.

Use `scripts/bench_record.sh --compare-bins` or
`scripts/compare_bins_by_bench.sh` with prebuilt baseline and candidate binaries.
This keeps the binaries fixed, rotates candidate order, inserts separate run IDs
per label, and stores raw NDJSON plus summary rows in `conflux_bench`.

JSON hot-path work that needs counter evidence should use the JSON perf helpers
instead of hand-rolled benchmark loops. Build candidates with
`scripts/json_perf_build_profiles.sh`, then run them with
`scripts/json_perf_run_conditions.sh` so calibration, fixed iteration counts,
compare-bins rotation, and `perf stat` capture stay aligned. Reports must keep
compiler/profile/condition groups separate and must not filter on instruction,
cycle, cache-miss, or branch-miss percentages. For cache-miss and branch-miss
signals, always report both the percentage delta and the absolute event-count
delta; a large percentage over a tiny event count is not equivalent evidence to
millions of extra misses.

PMR-aware JSON runtime work remains a revisit item. The first wide-row pass
showed parse-side wins on some file datasets, but dump/write-side regressions on
the same data shape, especially under O2/LTO, so it is not yet accepted as a
default performance change.

Minimum release acceptance matrix for a performance-sensitive patch that changes
hot-path behavior:

- `release-clang-libcxx`
- `release-gcc-stdcxx`
- `release-gcc16-stdcxx`

GCC 15 and GCC 16 release results are not interchangeable. The current
`release-gcc-stdcxx` lane is the no-LTO GCC baseline because GCC 15 ICEs with
LTO; `release-gcc16-stdcxx` enables LTO and must be measured as its own
before/after pair.

Minimum profiling/corroboration matrix when the mechanism is not already proven:

- `perf-clang-libcxx`
- `perf-gcc-stdcxx`

Release profiles exercise the production optimizer shape, including LTO where
the preset enables it. Perf profiles keep symbols and benchmark-only builds for
profiling and DB-backed evidence. Treat each compiler/profile pair as its own
baseline-vs-candidate comparison. Do not compare Clang deltas to GCC deltas or
release deltas to perf deltas as equivalent proof.

Debug, sanitizer, header-interface smoke, and P2996 compatibility lanes are
correctness or compatibility signals, not performance proof. P2996 reflection
performance claims need `release-p2996-gcc` evidence plus a matched
non-reflection/default baseline.

Suggested setup:

```sh
createdb conflux_bench || true
psql postgres://postgres@localhost/conflux_bench -f scripts/bench_db_migrate.sql
```

Build baseline and candidate into separate roots. Do not rebuild a binary after
recording its path. Use separate source worktrees so source-relative benchmark
fixtures, CMake generated files, and git metadata cannot be contaminated by
checking out the other revision.

Build only the benchmark target needed for the logical benchmark under test. Do
not build every benchmark binary by default; `/tmp` is often tmpfs, and GCC 16
release LTO artifacts are large enough to crowd out later pairs.

Never pass `-j`, `--parallel`, or an explicit job count. Let CMake/Ninja choose
the build parallelism. Do not run independent builds concurrently. Do not run a
build while any benchmark is running. Do not run two benchmark comparisons in
parallel; `compare-bins` already controls candidate order inside one comparison.

Target map:

- `uring_completion`: `conflux_uring_completion_bench`
- `file_io_sqe_storm`: `conflux_file_io_sqe_storm_bench`
- `file_copy_coro`: `conflux_file_copy_coro_bench`
- `tcp_increment`: `conflux_tcp_increment_coro_bench`
- `tls_tcp_increment_coro`: `conflux_tls_tcp_increment_coro_bench` when TLS is enabled
- `socket_raw`: `conflux_socket_raw_bench`
- `send_zc`: `conflux_send_zc_bench` when experimental SEND_ZC is enabled
- `work`: `conflux_work_benchmarks`
- `work_compile`: `conflux_work_compile_bench`
- `task_creation`: `conflux_task_creation_bench`
- `task_chain_composition`: `conflux_task_chain_composition_bench`
- `task_cancellation`: `conflux_task_cancellation_bench`
- `workpool_enqueue_dequeue`: `conflux_workpool_enqueue_dequeue_bench`
- `workpool_queue_mode_compare`: `conflux_workpool_queue_mode_compare_bench`
- `join_all_N`: `conflux_join_all_N_bench`
- `json`: `conflux_json_bench`
- `json_reflect`: `conflux_json_reflect_bench` when P2996 reflection is enabled
- `router`: `conflux_benchmarks`
- `template`: `conflux_template_bench`
- `crypto`: `conflux_crypto_bench`
- `http_server`: `conflux_http_server_bench`
- `http_server_concurrency`: `conflux_http_server_concurrency_bench`
- `db_coro`: `conflux_db_coro_bench` when DB support is enabled
- `db_params`: `conflux_db_params_bench` when DB support is enabled
- `db_pipeline`: `conflux_db_pipeline_bench` when DB support is enabled

Run one compiler/preset pair end-to-end, capture the run IDs and budget output,
then remove that pair's build directories before moving to the next pair. A
typical release order is `release-clang-libcxx`, `release-gcc-stdcxx`, then
`release-gcc16-stdcxx`; run perf pairs afterward only when profiling or
corroboration is needed.

```sh
# Baseline tree checked out at the chosen baseline commit:
#   /tmp/conflux-src-base
# Candidate tree checked out at the candidate patch:
#   /tmp/conflux-src-cand

BENCH=file_copy_coro
TARGET=conflux_file_copy_coro_bench
PRESET=release-clang-libcxx

cmake --preset "$PRESET" \
  -S /tmp/conflux-src-base \
  -B "/tmp/conflux-base/$PRESET"
cmake --build "/tmp/conflux-base/$PRESET" --target "$TARGET"

cmake --preset "$PRESET" \
  -S /tmp/conflux-src-cand \
  -B "/tmp/conflux-cand/$PRESET"
cmake --build "/tmp/conflux-cand/$PRESET" --target "$TARGET"
```

Compare each logical benchmark independently:

```sh
BENCH_REPS=9 BENCH_PIN_CPUS=0-3 scripts/compare_bins_by_bench.sh --yes \
  --dir "base-$PRESET:/tmp/conflux-base/$PRESET" \
  --dir "cand-$PRESET:/tmp/conflux-cand/$PRESET" \
  "$BENCH"
```

Repeat only for logical benchmarks affected by the patch. For example, P1 async
I/O completion work should normally include `uring_completion`,
`file_io_sqe_storm`, and `file_copy_coro`; add `tcp_increment`,
`tls_tcp_increment_coro`, `socket_raw`, or `send_zc` only when socket or
zero-copy paths are changed. P2 task/runtime work should include `work`,
`task_creation`, `task_chain_composition`, `task_cancellation`, `join_all_N`, and
the relevant WorkPool benches. P3 JSON work should include `json` and, when
enabled, `json_reflect`; add `http_server` or `db_*` rows only when the changed
path affects those integrations. Build only the relevant benchmark targets. If a
prior stable run is available, pass `--baseline-run-id` to reuse iteration
counts.

Capture the full compare-bins output for each run because the current wrapper
prints run IDs but does not write a stable machine-readable run-id mapping:

```sh
mkdir -p /tmp/conflux-bench/logs
# Example: append `2>&1 | tee /tmp/conflux-bench/logs/file_copy_coro-release-clang.log`
# to each compare-bins command.
```

Before relying on run metadata, fix or account for the current compare-bins
metadata limitation: `scripts/bench_record.sh --compare-bins` records
`COMPILER="clang++"` as a best-effort placeholder for every candidate. The run
label and build artifact path must therefore be treated as authoritative until
the script learns compiler metadata from the binary or label.

After each pair, run:

```sh
scripts/bench_check_budget.py \
  --baseline-run-id BASE_RUN_ID \
  --candidate-run-id CAND_RUN_ID \
  --json-out /tmp/conflux-bench/budget-BASE_RUN_ID-CAND_RUN_ID.json
```

After recording the run IDs, budget JSON, raw artifact path, and log path for
that compiler/preset pair, clean up its build roots before starting the next
pair:

```sh
rm -rf "/tmp/conflux-base/$PRESET" "/tmp/conflux-cand/$PRESET"
```

Acceptance rule:

- Candidate must not regress any budgeted variant beyond the existing budget.
- Noisy rows must be rerun before accepting.
- A microbenchmark win is not enough if the closest production-like benchmark
  regresses.
- If kernel round-trip, filesystem, socket, or scheduler overhead dominates a
  benchmark enough to hide a targeted allocation/layout change, add or extend a
  focused benchmark before accepting the performance claim.
- If the target bottleneck was not already isolated, capture profiler or
  hardware-counter evidence before changing code. Use microbenchmarks to compare
  candidate mechanisms, not to discover the whole-system bottleneck.
- For allocation-removal work, allocation count and allocated bytes are required
  evidence, not optional. RSS or allocator stats are required for longer-running
  I/O benches.
- For synchronization work, queue depth, lock/futex contention, context switches,
  CPU utilization, and tail latency are required where applicable.
- For compiler-dependent work, include vectorization reports, disassembly,
  generated-code inspection, or `perf stat` counters sufficient to confirm the
  expected mechanism.
- For template, reflection, modules, PGO/LTO, SIMD, or forced-inlining work,
  record binary-size and build-time movement.
- Benchmark notes must record allocator identity/config, CPU pinning, CPU
  governor, NUMA/topology, kernel version, filesystem/page-cache policy for file
  benches, background load, and artifact directory.

## Priority 1: Async I/O Completion Allocation

Problem:

- Current code evidence:
  - `src/uring/uring_completion.cxx`: `CompletionFn` has already been converted
    from `std::function<void(IoResult)>` to
    `conflux::detail::small_move_only_function<void(IoResult)>`.
  - `src/file_io/reader.cxx`: `FileReader::prepare_sqe<T>()` still heap-wraps
    `root::TaskSource<T>` in `std::shared_ptr` for most operations.
  - `src/uring/uring_timeout.cxx`: timeout submissions still use the
    `TaskSource` plus heap-holder shape.
- `src/file_io/reader.cxx` allocates a `std::shared_ptr<root::TaskSource<T>>`
  for most operations via `FileReader::prepare_sqe()`.
- Many operations allocate additional `shared_ptr` holders for path strings,
  `statx`, vectors of `iovec`, fixed-buffer holders, sockaddr storage, xattr
  state, timespecs, and related payloads.
- `src/uring/uring_timeout.cxx` follows the same `TaskSource` plus heap holder
  shape for timeout state.
- `src/work/root.cxx` already has `small_move_only_function`; the uring layer
  now reuses that move-only small-buffer dispatch for completion callbacks.

The likely cost is one or more heap allocations per submitted operation plus an
indirect completion call. This is the most concrete gap because it sits directly
in the io_uring submission/completion path.

Proposed design:

1. Treat the callback representation conversion as P1a and keep it behavior-only:
   no public API, compatibility mode, or scheduler policy change.
2. Measure whether `small_move_only_function` inline capacity covers the current
   hot callbacks; keep its fallback heap path for oversized captures.
3. Use the existing completion-table slot lifetime as the primary ownership
   boundary.
4. For P1b, update only simple file read/write completions to capture
   `TaskSource<T>` by move and remove the per-op shared wrapper on those paths.
5. Keep large or variable payloads behind a fallback allocation at first, but
   allocate them from per-slot storage or a ring-local slab with explicit
   per-slot reclamation. Do not use monotonic/batch reset unless every possible
   late CQE, cancellation CQE, linked timeout, multishot/event notification, and
   stale generation path has been drained.
6. Preserve generation checks, stale CQE rejection, zero-copy notification
   waiting, and `cancel_all()` behavior.

Initial slice:

- Start with one narrow operation family, preferably `FileReader::read_into`,
  and `FileReader::write_into`. Fixed-buffer, path/stat/iovec, timeout, and
  socket paths are later slices.
- Do not migrate path-based operations in the first patch; they have more
  lifetime payloads and are better as phase 2.
- Keep the old code path temporarily behind an internal switch only if needed to
  compare behavior during development. Do not expose a public compatibility API.

Correctness invariants:

- Completion fires exactly once for ordinary operations.
- Submit failure cleans up the record and produces the same task result as the
  current path.
- Partial read/write completion semantics stay unchanged.
- Negative CQE results map to the same error codes/messages as today.
- Cancellation produces the same observable task result as today.
- Stale generation CQEs are ignored.
- Payload storage outlives the kernel operation and completion callback.
- Reentrant completion callbacks cannot corrupt the free list or slot state.
- Linked timeout and zero-copy notification state cannot free payloads early.
- Multishot and provided-buffer ownership rules stay explicit where applicable.
- Shutdown and `cancel_all()` do not leak pending state.
- No callback can observe destroyed awaiter/task state.

Measurement:

- Primary: add or extend a focused benchmark for completion-table
  reserve/dispatch, small-function inline/spill behavior, and
  `FileReader::prepare_sqe()` allocation behavior. This is mandatory before
  accepting the change.
- Focused target: `uring_completion` for callback reserve/dispatch and
  `file_io_sqe_storm` for batched simple `FileReader::read_into` /
  `FileReader::write_into` SQE submission/completion.
- Production-like corroboration: `file_copy_coro`.
- Secondary: `json` rows that use `file_reader`.
- Metrics: median ns/iter, p99, allocation count/bytes, RSS or allocator stats
  for longer-running I/O benches, cycles/instructions/branch misses for the
  focused benchmark, and raw NDJSON retained in artifact dirs.

Expected risk: medium. The lifetime and cancellation surface is sensitive, but
the first slice can be kept small.

Rollback:

- Revert the new completion-record path and keep the existing shared ownership
  path if compare-bins does not show a stable win on both compilers.

## Priority 2: Task Control-Block Ownership

Problem:

- Current code evidence:
  - `src/work/root_tasks.cxx` and `src/work/root_core.cxx` store task state via
    `std::shared_ptr<ControlBlockInterface<T>>`.
  - `src/work/root_core.cxx` contains hot atomics, cold mutex/condition-variable
    state, and `small_move_only_function` callback slots whose layout and
    synchronization behavior must be preserved.
- `src/work/root_tasks.cxx` and `src/work/root_core.cxx` use
  `std::shared_ptr<ControlBlockInterface<T>>` heavily.
- Coroutine frame pooling exists behind `CONFLUX_WORK_CORO_FRAME_POOL` and
  `*-p5` release presets, but it is not the default path. The task control block
  itself remains a separate shared-ownership cost.
- `ControlBlockModel` has hot atomics, cold mutex/condition-variable state, and
  small-function callbacks; ownership changes must preserve that layout intent
  and prove they do not increase false sharing or lock pressure.

Proposed design:

1. Defer implementation until after P1, P3 instrumentation, and queue evidence
   have sharper attribution. Defer means later phase, not reject.
2. Add allocation telemetry around task creation/control-block paths first.
3. Use the async I/O completion work to learn the minimum ownership needed for
   task completion.
4. Prototype an intrusive refcounted control block for hot internally-owned
   tasks while preserving public `Task<T>` semantics.
5. Keep `shared_ptr` only for user-visible escape hatches and cross-thread state
   that genuinely needs shared ownership. The optimized path must become the
   default before acceptance; an internal compile option is only a temporary
   development comparison aid.
6. Before intrusive ownership work, run the existing frame-pool presets against
   the same release matrix to separate coroutine-frame allocation cost from
   control-block ownership cost.

Correctness invariants:

- Existing `Task`, `TaskSource`, `TaskControl`, join, abandon, cancellation, and
  exception propagation semantics remain unchanged.
- Refcounts are race-safe across producer/completer/awaiter paths.
- No use-after-free on abandoned tasks.
- Cross-thread await, abandon, cancellation, exception delivery, and destruction
  during pending completion have sanitizer/stress coverage.

Measurement:

- `work`
- `task_creation`
- `task_chain_composition`
- `workpool_enqueue_dequeue`
- Existing `CONFLUX_WORK_ALLOC_STATS` counters for diagnostic builds, plus
  release compare-bins for final acceptance and perf compare-bins for
  attribution/profiling.
- Optional `*-p5` release preset comparison as an attribution aid, not a
  substitute for the default release acceptance matrix.
- Lock/contention and cache-line evidence if the control-block layout changes.

Expected risk: high. This should follow, not precede, the smaller I/O completion
allocation work.

Rollback:

- Revert to the existing shared control-block path if the default optimized path
  does not pass correctness, sanitizer/stress coverage, and compare-bins gates.

## Priority 3: JSON Arena Over-Reservation

Problem:

- Current code evidence:
  - `src/json_parse.cxx`: parse paths reserve node, array-child, object-member,
    and string-arena capacity from input-size heuristics.
  - Duplicate-key tracking promotes to hash sets containing `std::string_view`
    values that can point into `string_arena`.
- The JSON parser is already performance-aware, but borrowed parse paths can
  over-reserve string arena space proportional to input size when only a small
  fraction of strings need copying.
- `src/json_parse.cxx` reserves `string_arena` to `input_view.size()` in both
  inplace and owned-storage parse paths.
- That reserve currently protects duplicate-key checking because promoted
  duplicate-key sets store `std::string_view`s that may point into
  `string_arena`. Removing the reserve without changing pointer stability would
  risk dangling views.
- The node, array-child, and object-member vectors also reserve from input-size
  heuristics. They are probably smaller than the string-arena issue, but should
  be measured in the same pass.

Proposed design:

1. Start with an instrumentation-only patch that records input size, copied
   string bytes, `string_arena` size/capacity, node/child/member size/capacity,
   duplicate-key hash promotions, and allocation count/bytes for JSON bench
   rows.
2. First try changing duplicate-key tracking to store stable descriptors,
   offsets, or hashes instead of raw `std::string_view`s into reallocating
   storage while preserving the existing contiguous `string_arena` storage
   format.
3. Keep copied/escaped strings stable without reserving for the entire input.
4. Add a small initial chunk sized from observed escaped-string demand, not total
   input size.
5. Treat a chunked string arena as a larger storage-format migration because
   `DocumentStorage` currently resolves strings through contiguous arena
   offsets.
6. Keep full-input reserve only for modes where the measured escape/duplicate
   profile proves it is faster and the memory cost is within budget.
7. Preserve duplicate-key policy semantics.
8. Keep this invisible to the public JSON API unless a separately approved,
   measured storage policy is justified.

Alternative:

- Use a two-tier strategy: current reserve for duplicate-heavy or escape-heavy
  documents detected early, chunked growth for ordinary borrowed inputs.

Measurement:

- `json` benchmark, especially corpus rows for large borrowed inputs, escaped
  strings, duplicate-key policy fixtures, and application-shaped payloads.
- Allocation count/bytes from existing JSON benchmark allocation instrumentation.
- Capacity slack: `capacity - size` for string arena, nodes, array children, and
  object members.
- Duplicate-key promotion rate and max object width.
- Throughput must not regress beyond budget.

Expected risk: low to medium. This is localized compared with the task/runtime
changes.

Rollback:

- Restore current reserve strategy if allocation savings cause parser throughput
  regressions on real corpora.

## Priority 4: WorkPool Queue Contention

Problem:

- Current code evidence:
  - `CONFLUX_WORK_QUEUE_STATS` and
    `scripts/work_queue_contention_evidence.sh` already exist.
  - `benchmarks/CMakeLists.txt` builds
    `conflux_workpool_queue_mode_compare_bench` from
    `workpool_enqueue_dequeue_bench.cxx` with
    `CONFLUX_WORKPOOL_QUEUE_MODE_COMPARE=1`; there is no separate
    `workpool_queue_mode_compare_bench.cxx` source file.
  - `src/work_impl.cxx` keeps steal-victim scans behind the
    `stealable_local_jobs` gate and still uses `admission_mtx_` for admission
    and shutdown correctness.
- The research recommends reducing synchronization and contention before moving
  to lower-level hints.
- The repo already has `CONFLUX_WORK_QUEUE_STATS`,
  `workpool_queue_mode_compare`, and `scripts/work_queue_contention_evidence.sh`.
- Existing notes and `src/work_impl.cxx` show that steal-victim scans are already
  gated behind `stealable_local_jobs`, and that disabling stealing by default
  would regress local-backlog redistribution.
- `admission_mtx_` is a correctness gate for `drain_and_stop()` and racing
  enqueue. Removing it needs a new admission-state protocol or producer epoch,
  not just a benchmark win.

Proposed design:

1. Treat queue contention as an investigation, not a default policy change.
2. Use queue-stat evidence to decide whether any production-like row is actually
   bottlenecked on admission, local deque, steal-victim, or futex paths.
3. Verify that the existing `stealable_local_jobs` gate removes steal-victim
   scans from external-only rows before proposing more scheduler work.
4. If contention remains, focus new investigation on the existing gate,
   `admission_mtx_`, and no-stealing admission behavior.
5. Do not remove `admission_mtx_` unless a reviewed protocol preserves
   `drain_and_stop()` correctness under racing producers.
6. Keep P4 as "queue contention evidence pass" until a specific bottleneck is
   proven.

Correctness invariants:

- `pending_` accounting remains exact.
- `drain_and_stop()` cannot observe an empty queue while an admitted producer has
  not published its pending work.
- Default stealing still redistributes worker-local backlog.
- No new unbounded queues or hidden blocking paths.
- Shutdown, cancellation, and abandoned task behavior remain unchanged.

Measurement:

- `workpool_queue_mode_compare` with queue stats enabled.
- `workpool_enqueue_dequeue` for historical continuity.
- Queue counters: admission/local/steal lock contention, steal attempts/hits,
  futex waits/wakes, runner-thread fairness, and tail latency.
- Release compare-bins for accepted default changes; perf queue-stat artifacts
  for diagnosis.
- Use `scripts/work_queue_contention_evidence.sh` for queue-stat artifacts; it
  configures `CONFLUX_WORK_QUEUE_STATS=ON` so the queue counters are meaningful.

Expected risk: medium. The benchmark surface is good, but scheduler semantics
and shutdown correctness are easy to damage.

Rollback:

- Revert any queue-mode or admission protocol change unless the production-like
  row improves without hurting local-backlog redistribution or shutdown tests.

## Priority 5: Toolchain and PGO Evidence

Problem:

- The research ranks release/LTO/PGO evidence ahead of manual branch hints,
  forced inlining, and SIMD.
- Release Clang presets use ThinLTO, release GCC default is currently no-LTO,
  and PGO gen/use presets already exist. The missing piece is an accepted
  representative training workload and evidence policy.
- Public performance claims can drift if they mix presets, ISA baselines, or
  profile-trained binaries without a controlled comparison.

Proposed design:

1. Record CMake cache, LTO mode, compiler version, standard library, `-march` or
   target CPU, and binary size for every release comparison.
2. Use existing PGO presets only after defining representative training
   workloads for the target component.
3. Compare PGO binaries against equivalent non-PGO release binaries on the same
   workload and host.
4. Prefer PGO over manual `[[likely]]`, `always_inline`, or hot/cold attributes
   where branch frequencies or layout are the intended mechanism.
5. Promote PGO presets only if the training data is reproducible enough for CI or
   release-candidate evidence.

Candidate training workloads:

- `json` corpus and route payloads for JSON parser/dump paths.
- `http_server` and `http_server_concurrency` for branch-heavy request paths.
- `file_copy_coro` for file/runtime flow.
- `work` and `task_chain_composition` for scheduler/task paths.

Measurement:

- Same release compare-bins matrix as other performance work.
- PGO vs non-PGO deltas per compiler where tooling exists.
- Build-time, link-time, binary-size, and profile-generation overhead.
- `perf stat` counters when the claim is branch/layout improvement.

Expected risk: medium. The runtime upside can be real, but stale or
unrepresentative profiles can regress codegen.

Rollback:

- Keep non-PGO release presets as the default unless PGO evidence is stable and
  reproducible for the release process.

## Priority 6: CPU Specialization and Runtime Dispatch

Problem:

- SIMD support exists, but public performance claims are sensitive to compile
  flags and selected preset.

Proposed design:

1. First document CPU baseline for performance claims: compiler, standard
   library, `-march`/ISA, host CPU, kernel, allocator, and preset.
2. Add runtime dispatch only for kernels where benchmark data shows meaningful
   ISA-dependent wins.
3. Keep scalar fallback and compile-time feature gates.
4. Compare explicit SIMD against the best scalar release baseline, after PGO or
   compiler auto-vectorization evidence has been checked.
5. Include vectorization reports or disassembly when the expected mechanism is
   compiler vectorization.

Candidate kernels:

- JSON string scan/dump.
- Future JSON structural scan.
- HTTP header scan helpers if profiling identifies them as hot.

Measurement:

- `json`
- `http_server`
- `http_server_concurrency`
- Optional external `perf stat` for cycles/instructions/branches/cache misses.

Expected risk: medium. Dispatch can improve portability of perf claims, but it
adds code size and testing matrix cost.

Rollback:

- Keep compile-time paths as the fallback and disable dispatch per target if it
  is noisy, slower, or bloats binaries without a matching release win.

## Priority 7: External Proof Package

Problem:

- The project has strong internal benchmark machinery, but pre-v1 public claims
  need raw reproducible evidence.

Proposed design:

1. Tier A, internal perf-sensitive patch: preserve compare-bins run IDs, budget
   JSON, allocation/counter evidence, and affected-benchmark summaries.
2. Tier B, public performance claim: preserve raw NDJSON, run IDs, host,
   compiler, kernel, allocator, CMake cache, CPU governor/load, and benchmark
   inputs.
3. Tier C, release benchmark page: publish summaries plus raw evidence, external
   comparisons, methodology, and best/worst honesty.

Minimum evidence for the async I/O allocation work:

- Clang release compare-bins: baseline and candidate run IDs.
- GCC 15 release compare-bins: baseline and candidate run IDs.
- GCC 16 release compare-bins: baseline and candidate run IDs.
- Clang perf compare-bins: baseline and candidate run IDs.
- GCC perf compare-bins: baseline and candidate run IDs.
- `bench_check_budget.py` JSON outputs.
- Raw NDJSON artifact path.
- Allocation count/bytes evidence for the focused completion benchmark.
- RSS or allocator-stat evidence for longer-running I/O corroboration.
- Short interpretation: variants improved, unchanged, noisy, regressed.

## Proposed Ticket Order

1. P1a: completion callback representation only; no behavior change. Partially
   implemented by the current `CompletionFn = small_move_only_function` change.
2. P1b: simple `FileReader::read_into` and `FileReader::write_into`
   completions capture `TaskSource` by move.
3. P1c: timeout path conversion.
4. P1d: fixed-buffer, iovec, path, and other larger payload storage.
5. P3a: JSON capacity and duplicate-key instrumentation.
6. P3b: duplicate-key stable descriptors.
7. P3c: reduce `string_arena` over-reserve.
8. P4a: queue contention evidence pass only.
9. P5a: PGO/toolchain policy and representative workload evidence.
10. P2a: task allocation attribution.
11. P2b: intrusive control-block prototype.
12. P6a: CPU dispatch or SIMD only after scalar, PGO, and generated-code
    evidence.
13. P7a: external proof package for public claims.

Implementation priority ranking:

1. P1 async completion allocation.
2. P3 JSON arena over-reservation.
3. P4 queue contention evidence.
4. P5 PGO/toolchain policy.
5. P2 task control-block ownership.
6. P6 CPU dispatch.
7. P7 external proof packaging.

## Open Questions

- Should the first completion-record slice target file read/write or socket recv
  completions?
- Should the uring callback representation reuse `small_move_only_function` from
  root internals, or should uring own a narrower completion-only type?
- Should `bench_record.sh --compare-bins` infer compiler metadata from labels,
  binary `--bench-info`, or CMake cache?
- Do we need a dedicated allocation benchmark for `CompletionTable` dispatch and
  `FileReader::prepare_sqe()` before implementation?
- Should the JSON first patch be instrumentation-only so capacity slack is
  visible before replacing the reserve strategy?
- Which WorkPool queue profiles are representative enough to justify default
  scheduler changes?
- What training workload is stable enough before promoting or using an existing
  PGO preset?

## Non-Goals

- No broad task runtime rewrite in the first patch.
- No lock-free reclamation work unless profiling shows a synchronization
  bottleneck and the reclamation design is reviewed separately.
- No SIMD structural JSON parser rewrite until current JSON corpus benchmarks
  identify parse throughput as the limiting factor.
- No public API compatibility aliases or old-path preservation beyond temporary
  internal switches needed for measurement.
- No WorkPool default queue-mode change from microprofile evidence alone.
- No PGO, branch-hint, forced-inline, or ISA-dispatch public claim without a
  matching non-PGO/scalar/default-release baseline.
