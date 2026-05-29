# JSON direct-to-struct benchmark results template

Use this file as the per-branch/per-patch notebook for
`conflux_json_direct_struct_bench`. Copy it into a dated report before each
measurement run.

## Run metadata

| Field | Value |
|---|---|
| Date / run id |  |
| Branch / patch |  |
| Baseline commit or archive |  |
| Compiler / version |  |
| Standard library |  |
| CMake preset / flags |  |
| Build type / LTO / PGO |  |
| CPU / ISA / governor |  |
| Kernel / OS |  |
| Allocator |  |
| Thermal / affinity / NUMA controls |  |
| Benchmark command |  |
| Perf command |  |

## Top-line comparison

| Group | Baseline ns/iter | Candidate ns/iter | Delta | Baseline alloc B/iter | Candidate alloc B/iter | Notes |
|---|---:|---:|---:|---:|---:|---|
| `order/medium/declaration` |  |  |  |  |  |  |
| `order/medium/shuffled` |  |  |  |  |  |  |
| `order/wide64/declaration` |  |  |  |  |  |  |
| `order/wide64/shuffled` |  |  |  |  |  |  |
| `unknown/wide64/interleaved_ignore` |  |  |  |  |  |  |
| `duplicate/name/reject` |  |  |  |  |  |  |
| `duplicate/name/last_wins` |  |  |  |  |  |  |
| `numeric/point_cloud/integers` |  |  |  |  |  |  |
| `numeric/point_cloud/fixed_decimal` |  |  |  |  |  |  |
| `numeric/point_cloud/scientific` |  |  |  |  |  |  |
| `strings/owned/plain_long` |  |  |  |  |  |  |
| `strings/owned/escaped_long` |  |  |  |  |  |  |

## Hardware counters

| Group/filter | Cycles | Instructions | IPC | L1I misses | Branch misses | LLC misses | Top no-children symbols |
|---|---:|---:|---:|---:|---:|---:|---|
| `order/` |  |  |  |  |  |  |  |
| `duplicate/` |  |  |  |  |  |  |  |
| `numeric/` |  |  |  |  |  |  |  |
| `strings/` |  |  |  |  |  |  |  |

## Decision notes

| Hypothesis | Evidence | Decision |
|---|---|---|
| Member-order path is the bottleneck |  |  |
| Duplicate rejection is materially expensive |  |  |
| Number lexing dominates fixed numeric arrays |  |  |
| String escaping/scanning dominates string rows |  |  |
| Allocation policy changed runtime unfairly |  |  |
| Candidate should be integrated |  |  |
