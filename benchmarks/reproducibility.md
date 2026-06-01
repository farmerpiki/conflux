# Benchmark Reproducibility

This page defines how benchmark evidence artifacts should be produced when a
performance claim needs one. It is not a performance claim. Final public
benchmark capture is performed only after release-candidate
source/API/docs/example shape is frozen, so published graphs match the commit
users can actually build.

## Candidate Perf Methodology

Use this workflow for implementation-candidate performance work. The goal is to
separate correctness, build/profile preparation, measurement, and reporting so
results are comparable across candidates and future runs.

1. Build each candidate in its own worktree.

   Keep one base worktree and one worktree per candidate. Use `/tmp` for build
   trees and generated profile data. Do not reuse a dirty build tree when the
   candidate source changed.

2. Run focused correctness tests before timing.

   Run only the relevant component tests for every candidate. If a test is
   already broken in the base tree, record it and ignore it for this perf pass.
   Do not change expectations or behavior just to make a perf batch pass.

3. Prepare benchmark binaries before measuring.

   Build the normal release profile, the O2/LTO profile, the PGO-generate
   profile, and the PGO-use profile before starting compare-bins measurement.
   For JSON candidates, use:

   ```sh
   scripts/json_perf_build_profiles.sh \
     --tree base:/tmp/conflux-jsonpatch-<id>-base \
     --tree candidate:/tmp/conflux-jsonpatch-<id>-candidate
   ```

   The preparation step owns PGO. PGO-generate binaries must first run
   calibration with `--iterations 0`, then run training again with the discovered
   fixed iteration count. Do not train PGO-use from the short discovery run
   itself; that produces profiles for calibration behavior rather than benchmark
   behavior.

4. Measure only already-built binaries.

   The measurement script must not build, train PGO, or include PGO-generate
   binaries. It should compare normal release, O2/LTO release, and PGO-use
   release binaries together for a single compiler/stdlib and benchmark, so rows
   can answer questions such as "candidate A regresses on clang but is stable on
   GCC" or "candidate B wins only under GCC 16 PGO".

   ```sh
   BENCH_PIN_CPUS=2 scripts/json_perf_run_conditions.sh \
     --tree base:/tmp/conflux-jsonpatch-<id>-base \
     --tree candidate:/tmp/conflux-jsonpatch-<id>-candidate
   ```

   Use `BENCH_REPS`/`JSON_PERF_REPS` for repeated measurements. `5` is the
   default for candidate work. `perf stat` capture is one run per binary; more
   repetitions usually do not add useful information for counters.

5. Keep iterations fixed inside a comparison.

   First run calibration separately. Then pass the calibration run id into
   compare-bins so all candidates in that compiler/stdlib/benchmark group use
   the same iteration counts. Never compare rows where each binary independently
   chose a different iteration count.

6. Control the host as much as practical.

   Pin benchmark launches with `BENCH_PIN_CPUS`. Prefer a performance governor
   and pinned VM vCPUs when available. Do not run independent builds and
   benchmarks concurrently; build heat and CPU migration are large enough to
   hide small effects.

7. Report from Postgres with stable grouped rows.

   Use the report helper instead of ad hoc SQL. It keeps fixed-width columns,
   groups by compiler/stdlib, profile condition, and benchmark, and keeps
   per-variant rows while dropping negligible effects by default.

   ```sh
   scripts/json_perf_report.py /tmp/conflux-json-perf-artifacts/<stamp>
   scripts/json_perf_report.py /tmp/conflux-json-perf-artifacts/<stamp> \
     --bench json_storage \
     --profile gcc16 \
     --wall-threshold-pct 1
   ```

   Row filtering is based only on wall-time effect. Instruction and cycle
   deltas are evidence for the rows that remain, but they never decide which
   rows are hidden. Use `--include-negligible` only when auditing noise. Tighten
   wall thresholds as benchmark stability improves.

8. Interpret wall time and counters together.

   A candidate can still be a win when wall time is flat but instructions drop
   at similar or lower cycles. Treat lower instructions as useful signal when
   p50/p10/p99 are within noise, especially for changes meant to simplify hot
   code paths. Treat higher instructions with flat wall time as a warning: it may
   become visible after unrelated layout or optimizer changes.

9. Diagnose large wins or regressions with filtered sub-bench perf.

   Whole-binary `perf stat` is useful for initial triage, but it can hide which
   variant is responsible when one benchmark executable emits many rows. When a
   row is a major win/loss, rerun that exact row or row family with the benchmark
   `--filter` option and collect per-sub-bench perf counters. Keep cycles,
   instructions, branches, branch-misses, cache-references, cache-misses,
   L1-dcache-loads, L1-dcache-load-misses, dTLB-loads, and dTLB-load-misses.

   ```sh
   BENCH_PERF_EVENTS='cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses' \
     scripts/bench_perf_stat.py \
       --perf-json /tmp/conflux-perf/<profile>.<candidate>.<row>.perf.json \
       -- taskset -c 2 /tmp/<tree>/build/<profile>/benchmarks/conflux_json_bench \
          --json --filter 'parse/long_strings'
   ```

   Report the base row scale and candidate deltas for ns/op, instructions/op,
   cycles/op, cycles/instruction, branch-misses/op, cache-misses/op, and L1/TLB
   misses/op. If instructions decrease but cycles or CPI increase, look for
   branch prediction, I-cache/layout, and data-cache effects before rejecting the
   candidate. A targeted source change such as a small `[[likely]]`/`[[unlikely]]`
   annotation or layout adjustment is valid follow-up only after filtered perf
   shows the failure mode.

10. Preserve skipped lanes explicitly.

   If a compiler ICEs under LTO or PGO, skip that lane and report the skip. Do
   not block the whole candidate batch when other compilers produce usable
   evidence.

Record raw command lines with each artifact:

```sh
cmake -S . -B /tmp/conflux-bench -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/conflux-bench
./scripts/run-build-artifact.sh /tmp/conflux-bench/benchmarks/conflux_benchmarks
```

Attach environment metadata:

- Conflux commit and exact external comparison commits or versions.
- Compiler and CMake versions.
- CPU model, microcode when available, kernel version, and libc.
- Governor, turbo state, CPU pinning, NUMA placement, and background-load notes.
- Warmup policy and exact benchmark filters, including any sub-bench `--filter`
  used for diagnostic perf reruns.
- For live-kernel rows, attach `perf stat` events or wrapper output where
  practical: cycles, instructions, context switches, cache/TLB misses, and
  relevant syscall counts. `scripts/bench_perf_stat.py` can annotate benchmark
  NDJSON rows without changing the benchmark binary.

Use a six-run policy for public comparisons: compare the external best run with
the Conflux worst run, and publish raw min, median, and max for both systems.
Keep bulky raw JSON/CSV artifacts outside the tracked source tree unless they
are tiny smoke fixtures.

Do not refresh release graphs during active implementation churn. Refresh them
from the final benchmark artifact set immediately before tagging, after the
source commit, public examples, minimum toolchain baseline, and benchmark cases
are settled.

## Artifact Schema

JSON rows should include:

```json
{
  "name": "suite/case",
  "system": "conflux",
  "commit": "git-sha",
  "compiler": "g++ 16",
  "cmake": "4.0",
  "kernel": "Linux 6.x",
  "run": 1,
  "unit": "ns",
  "value": 0,
  "warmup_iterations": 0
}
```

CSV files should use the same fields in header order:

```text
name,system,commit,compiler,cmake,kernel,run,unit,value,warmup_iterations
```
