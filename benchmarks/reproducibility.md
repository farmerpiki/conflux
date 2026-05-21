# Benchmark Reproducibility

This page defines how benchmark proof artifacts should be produced. It is not a
performance claim. Final public benchmark capture is performed only after
release-candidate source/API/docs/example shape is frozen, so published graphs
match the commit users can actually build.

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
- Warmup policy and exact benchmark filters.

Use a six-run policy for public comparisons: compare the external best run with
the Conflux worst run, and publish raw min, median, and max for both systems.
Keep raw JSON/CSV artifacts in the proof repository unless they are tiny smoke
fixtures.

Do not refresh release graphs during active implementation churn. Refresh them
from the final proof-repository run immediately before tagging, after the source
commit, public examples, minimum toolchain baseline, and benchmark cases are
settled.

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
