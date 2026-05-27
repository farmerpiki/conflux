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
selected compiler/CMake preset cannot provide the required module support.

Generated headers are release artifacts for compatibility consumers. They are
generated from module sources during artifact staging and are not tracked as
hand-maintained source.

## Package Components

Mock liburing header installs publish only `core`, `types`, `json`, and
`file_io_sync` as internal compile evidence for generated header artifacts. They
intentionally do not publish real-liburing runtime-facing components such
as `work` or `http`.

Real-liburing installs may publish runtime-facing components such as `work`
and `http`; consumers must be able to find `liburing` through `pkg-config`.

DB-off installs must not ship generated `db`/`pg` headers and must not advertise
`db` or `pg` package components.

## Toolchain Baseline

Current prerelease lanes use CMake 3.30+ with Ninja and the checked-in compiler
presets. Downgrading the baseline requires a green configure/build/package
smoke lane, not just syntax compatibility.

## Evidence

Runtime and benchmark proof belongs in release evidence or the separate proof
repository, not as bulk logs in this source tree. Use
`docs/releases/evidence-template.md` for small manifests.

Final public proof capture is intentionally deferred until release-candidate
source freeze. During normal prerelease churn, this page describes the evidence
contract only; it is not a performance claim and should not point at stale graphs
from a source/API shape that will not be tagged.

## Known Unsupported Areas

- non-Linux platforms
- runtime-facing/http packages built only with mock liburing
- arbitrary C++ module toolchains outside the checked presets
- benchmark claims without same-machine artifacts

## Cheap Checks

```sh
python3 scripts/check_no_std_streams.py
python3 scripts/check-first-contact-public-dialect.py
python3 scripts/check-component-map.py
python3 scripts/check-cost-lifetime-docs.py
python3 scripts/check-planning-state.py
python3 scripts/check-release-docs.py
python3 scripts/check-package-docs.py
python3 scripts/check-release-notes.py
bash scripts/check-package-config.sh
bash scripts/stage-release-artifacts.sh --stage-dir /tmp/conflux-release-artifacts/stage --no-tarball
```
