# Prerelease Status

This dashboard summarizes the current public-preview contract. It is not a
replacement for the release checklist.

## Preview Scope

- `core`
- `json`
- `file_io_sync`
- `http` and runtime-facing components such as `work` only from real-liburing installs
- `db` only when DB is enabled and libpq is available

## Interface Modes

`MODULE_INTERFACE` is the prerelease primary interface for source consumption
and development. It remains toolchain-sensitive and must fail clearly when the
selected compiler/CMake preset cannot provide the required module support. The
preview support contract is the tested preset matrix, not arbitrary module
compiler combinations.

Generated headers are release artifacts for compatibility consumers. They are
generated from module sources during artifact staging and are not tracked as
hand-maintained source. Header-interface support is best-effort outside the
components, package modes, and compiler versions covered by release evidence.

## Package Components

Liburing-free header installs publish only `core`, `types`, `json`, and
`file_io_sync` as generated header artifact evidence. They intentionally do not
publish runtime-facing components such as `work` or `http`.

Real-liburing installs may publish runtime-facing components such as `work`
and `http`; consumers must be able to find `liburing` through `pkg-config`.

DB-off installs must not ship generated `db`/`pg` headers and must not advertise
`db` or `pg` package components.

## Toolchain Baseline

Current prerelease support lanes use CMake 4.2+ with Ninja and the checked-in
compiler presets. The project files may accept older CMake versions for local
experiments, but older versions are not supported unless a green
configure/build/package smoke lane is attached to the release evidence.

The intended compiler floor for the preview is GCC 15, GCC 16, and Clang 21.
GCC 15 is the no-LTO release lane; GCC 16 and Clang 21 are the LTO-capable
release lanes; GCC 16 is the current reflection lane.

## Evidence

Current evidence is attached at https://github.com/farmerpiki/conflux_evidence
commit `a3fa450` for Conflux `v0.1.0-preview` (`f0f511b`).

Attached lanes: release-json/http-api/web-server SKU closures (full sanitizers
on release-json), JSONTestSuite + differential smoke, HTTP throughput benchmarks
(4 workloads, 6 rounds, recorded under source-equivalent commit `6466c3cb`),
JSON throughput benchmarks (6 rounds, 3 compilers: clang/gcc/gcc16), Docker
conformance (h2spec/wstest/spectral).

Not attached in this preview: PostgreSQL evidence, HTTP/2 multiplexing /
WebSocket throughput / static file benchmarks, exact-commit formal HTTP
benchmark proof, formal JSON benchmark proof.

Benchmark evidence is in `conflux_evidence/<sha>/`. Docker conformance raw
artifacts are in `conflux_evidence/conformance-docker/`. Use
`conflux_proof/scripts/produce-evidence.sh` to regenerate structured evidence
or `scripts/collect-evidence.sh` + `scripts/run-conformance-docker.sh` for
benchmarks and conformance separately.

## Known Unsupported Areas

- non-Linux platforms
- runtime-facing/http packages without a real liburing dependency
- CMake versions older than the release-evidenced baseline
- arbitrary C++ module toolchains outside the checked GCC 15, GCC 16, and
  Clang 21 presets
- benchmark claims without same-machine artifacts
- protocol, OpenAPI, JSONTestSuite, differential-parser, or JSON benchmark
  claims without attached external proof artifacts

## Cheap Checks

```sh
python3 scripts/check_no_std_streams.py
python3 scripts/check-first-contact-public-dialect.py
python3 scripts/check-component-map.py
python3 scripts/check-cost-lifetime-docs.py
python3 scripts/check-planning-state.py
python3 scripts/check-release-docs.py
python3 scripts/check-release-skus.py
python3 scripts/check-package-docs.py
python3 scripts/check-release-notes.py
bash scripts/check-package-config.sh
bash scripts/stage-release-artifacts.sh --stage-dir /tmp/conflux-release-artifacts/stage --no-tarball
```
