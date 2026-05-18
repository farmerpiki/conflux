# Contributing

Conflux is currently pre-v1. Public APIs may still change before v1 when the
change simplifies the final API, fixes a wrong contract, or improves the runtime
model.

Before sending changes:

- configure and build with one of the checked-in CMake presets;
- run the relevant test binary or `ctest` lane for the changed component;
- compile examples when public API or package targets change;
- use release/perf presets for benchmark claims;
- update docs when public names, behavior, packaging, or security posture
  changes.

Tests are contracts. Do not relax an existing test just to fit an
implementation change; add coverage for new behavior when the contract changes.
The fuller policy is in [`docs/project-policy.md`](docs/project-policy.md).
