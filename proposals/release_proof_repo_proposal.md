Status: proposed release blocker; defer final capture until release-candidate source freeze.
Branch: `release/proof-repo-final-evidence`

# Proposal: External Release Proof Repository

## Problem

Conflux should not ship bulky runtime, benchmark, flamegraph, and raw log
artifacts in the main source repository. Most users need source, examples,
package metadata, and concise release notes. Skeptical users, maintainers, and
performance reviewers need the opposite: exact commands, machine metadata, raw
runs, skipped-test notes, and enough artifacts to reproduce or falsify public
claims.

Keeping proof in-tree would pollute history and create noisy churn whenever
hardware, compiler, kernel, or benchmark graphs change. Keeping proof entirely
private weakens release trust. The correct split is a small evidence manifest in
this repository and a separate proof repository for heavy artifacts.

## Decision

Create a separate GitHub repository, preferably named `conflux-proof`, as the
canonical home for release evidence.

The main `conflux` repository keeps only:

- concise benchmark and proof summaries;
- release-note links to immutable proof runs;
- small machine-readable manifests or manifest templates;
- documentation that explains how to reproduce the evidence;
- no raw bulky logs, perf data, flamegraphs, or repeated benchmark CSV/JSON.

The proof repository stores:

- configure/build/test/package logs;
- runtime preflight results and capability probes;
- benchmark raw CSV/JSON/NDJSON;
- generated performance graphs;
- perf/stat/flamegraph artifacts;
- compiler, CMake, libc, kernel, CPU, governor, turbo, NUMA, and pinning info;
- exact command lines and environment variables;
- external comparison source revisions and build flags;
- skipped-test or unsupported-host notes;
- release candidate summary reports.

## Timing

Do not produce the final public proof run during active API, examples, docs, or
benchmark-shape churn. Until the release candidate is otherwise ready, maintain
only scripts, templates, and provisional local smoke artifacts.

Final proof capture happens after:

1. public source formatting and human-readable cleanup are complete;
2. public API spelling and examples are frozen for the preview;
3. module/header artifact shape is frozen;
4. minimum compiler/CMake baseline has been lowered as far as the tree actually
   supports;
5. benchmark cases and graph scripts are final enough that rerunning them will
   not invalidate public documentation;
6. release notes have placeholders for proof-run links and claims.

This keeps the evidence aligned with the actual released source state and avoids
publishing graphs for a shape that no user can build.

## Proof repository layout

```text
conflux-proof/
  README.md
  runs/
    2026-05-XX-0.1.0-preview-rc1/
      manifest.json
      environment.txt
      conflux-commit.txt
      proof-repo-commit.txt
      commands.sh
      build/
        configure.log
        build.log
        ctest.log
        package-smoke.log
        header-interface-smoke.log
      runtime/
        io_uring-preflight.log
        capability-probes.json
      fuzz-security/
        fuzz-smoke.log
        http-security-corpus.log
      benchmarks/
        raw/
        csv/
        json/
        ndjson/
        summaries/
        graphs/
      perf/
        stat/
        record/
        flamegraphs/
      external/
        drogon/
        uwebsockets/
        cpp-httplib/
        boost-beast/
  scripts/
    collect-env.sh
    run-build-proof.sh
    run-runtime-proof.sh
    run-benchmarks.sh
    summarize.py
```

## Required manifest

Every proof run must have a machine-readable manifest:

```json
{
  "project": "conflux",
  "release": "0.1.0-preview",
  "candidate": "rc1",
  "date_utc": "2026-05-XXT00:00:00Z",
  "conflux_commit": "<sha>",
  "proof_repo_commit": "<sha>",
  "source_tree_status": "clean",
  "preset_matrix": ["release-clang-libcxx", "release-gcc-stdcxx"],
  "interface_modes": ["MODULE_INTERFACE", "HEADER_INTERFACE artifact"],
  "machine": {
    "cpu": "<model>",
    "ram": "<capacity/speed if known>",
    "kernel": "<uname -a>",
    "libc": "<version>",
    "governor": "performance|powersave|unknown",
    "turbo": "on|off|unknown",
    "numa": "<placement>",
    "pinning": "<taskset/cset policy>"
  },
  "toolchain": {
    "compiler": "<name/version>",
    "cmake": "<version>",
    "ninja": "<version>",
    "python": "<version>",
    "liburing": "<version>",
    "openssl": "<version or disabled>",
    "libpq": "<version or disabled>"
  },
  "status": {
    "configure": "pass",
    "build": "pass",
    "tests": "pass",
    "package_smoke": "pass",
    "runtime_preflight": "pass|skip:<reason>",
    "benchmarks": "pass|skip:<reason>"
  },
  "claims": [
    {
      "claim": "<short release-note claim>",
      "artifact": "benchmarks/summaries/<file>",
      "raw": "benchmarks/raw/<file>"
    }
  ]
}
```

## Benchmark policy

Public comparison claims must use the same host, same benchmark window, and raw
published runs. For direct external comparisons, run each competitor and Conflux
six times, publish all six raw runs, and summarize using:

- external best run;
- Conflux worst run;
- min/median/max for both;
- command lines, source commits, compiler flags, and environment metadata.

This intentionally biases the public headline against Conflux, reducing room for
claims that the benchmark was cherry-picked. It does not replace full raw data;
it defines the conservative headline number.

## Main repository doc wording

Before release, main-repository docs should use deferred wording:

- `final proof capture is performed after release-candidate source freeze`;
- `current docs describe the evidence contract, not a performance claim`;
- `benchmark graphs are generated from the final proof repository run`;
- `raw logs and bulky artifacts are external to this source tree`;
- `release notes may cite proof paths only after the final run is attached`.

Avoid wording that implies current checked-in benchmark notes are the final
public performance evidence.

## Acceptance criteria

- `conflux-proof` exists or the release notes name its intended URL before the
  final candidate is tagged.
- `docs/release-checklist.md` requires a proof repository commit/path for every
  runtime or performance claim.
- `docs/prerelease-status.md` states that final proof capture is deliberately
  deferred until release-candidate source freeze.
- `docs/releases/0.1.0-preview.md` contains placeholders for proof-run links and
  refuses benchmark claims without attached same-machine artifacts.
- `benchmarks/reproducibility.md` documents the six-run conservative comparison
  rule and the final-capture timing.
- Raw benchmark/log artifacts remain out of the main source repository.
- The main release tarball contains source and small manifests only, not bulky
  proof artifacts.

## Non-goals

- Do not add generated graphs to the main repository.
- Do not make proof runs part of normal local development.
- Do not require users uninterested in performance proof to clone the proof
  repository.
- Do not treat pre-freeze benchmark output as public release evidence.
