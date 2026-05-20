# Prerelease Status

This dashboard summarizes the current public-preview contract. It is not a
replacement for the release checklist.

## Preview Scope

- `core`
- `json`
- `file_io_sync`
- `http` and `runtime` only from real-liburing installs
- `db` only when DB is enabled and libpq is available

## Interface Modes

`HEADER_INTERFACE` is the prerelease consumption baseline. `MODULE_INTERFACE`
remains toolchain-sensitive and must be validated with the matching preset.

## Package Components

Mock-liburing header installs publish only `core`, `types`, `json`, and
`file_io_sync`. They intentionally do not publish `runtime` or `http`.

Real-liburing installs may publish `runtime` and `http`; consumers must be able
to find `liburing` through `pkg-config`.

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

## Known Unsupported Areas

- non-Linux platforms
- runtime/http packages built only with mock liburing
- arbitrary C++26 module toolchains outside the checked presets
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
bash scripts/check-package-smoke-liburing-free.sh
```
