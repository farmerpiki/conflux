# Release Policy

Conflux is pre-v1 software and does not tag a public prerelease unless the
selected release SKU and its evidence are explicit.

## Required Before Tagging

- The release SKU is named, for example `release-json`, `release-http-api`, or
  `release-web-server`.
- The package component list matches the selected SKU.
- The README and selected docs provide a first-contact entrypoint for the SKU.
- The selected examples build.
- Release artifact staging passes and validates the staged artifact.
- Package smoke tests cover the selected module/header interface contract.
- Capability, compile-time, binary-size, and known-limitation reports are
  attached when the release claims them.
- No known P0 security, parser, packaging, or adversarial behavior failure is
  open for the selected SKU.

## Evidence

Release evidence must be reproducible from the tagged source and staged release
artifacts. Final benchmark or conformance claims require raw data, commands,
commits, configs, and patches that can be rerun.

Scratch benchmark output is not release evidence. Public proof belongs only in
the release artifact/evidence flow described by
[`docs/release-checklist.md`](docs/release-checklist.md).

Security-impacting fixes may break API before 1.0 when the safe behavior cannot
be shipped compatibly. Safe defaults take priority over benchmark modes, and
unsafe or benchmark-specific modes must never be defaults.
