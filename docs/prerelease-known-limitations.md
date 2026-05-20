# Prerelease Known Limitations

The first preview is scoped to package consumption and build evidence in this
repository.

- Runtime proof and benchmark proof live in separate evidence artifacts.
- C++26 module mode remains sensitive to the compiler, standard library, and
  CMake import-std support.
- `CONFLUX_USE_MOCK_LIBURING=ON` is build evidence only. It does not prove that
  the host can run runtime-facing components.
- DB examples require `CONFLUX_ENABLE_DB=ON`, libpq headers, and the `conflux_db`
  target. When DB is disabled, DB examples are skipped even if libpq is installed
  on the host.
- Header-interface package smoke is the baseline consumer lane and should not
  require import-std discovery.
