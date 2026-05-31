# Prerelease Known Limitations

The first preview is scoped to package consumption and build evidence in this
repository.

- Runtime proof and benchmark proof live in separate evidence artifacts.
- Module and import-std support remains sensitive to the compiler, standard
  library, and CMake import-std support. The preview support matrix is limited
  to the checked GCC 15, GCC 16, and Clang 21 lanes with CMake 4.2+ and Ninja.
  Optional reflection currently belongs to the GCC 16 P2996 lane; optional
  standard-SIMD targets are C++26-gated.
- `CONFLUX_USE_MOCK_LIBURING=ON` is build evidence only. It does not prove that
  the host can run runtime-facing components.
- DB examples and generated DB headers require `CONFLUX_POSTGRES_PROVIDER=LIBPQ`, libpq
  headers, and the `conflux_pg` target. When DB is disabled, DB examples and
  DB headers are skipped even if libpq is installed on the host.
- Header-interface package smoke is a generated compatibility-artifact lane and
  should not require import-std discovery. It is not the design center for new
  API work and is best-effort outside the component/toolchain matrix covered by
  release evidence.
