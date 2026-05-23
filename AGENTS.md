communication:clear+precise+succint
bench run->release
bench/examples/tests->!sandbox
build artifacts: use `./scripts/run-build-artifact.sh <artifact>` outside sandbox, from an EXISTING profile only; prepend `timeout 2s ` when running examples
helper: if expected build artifact is rejected/missing, update `scripts/run-build-artifact.sh` only for existing profiles/path patterns, never for ad hoc profile names
sanitizer build/tests->!sandbox (Catch2 discovery executes test bins during build)
builds: never pass jobs/-j/--parallel; let Ninja/CMake auto-detect jobs; never run independent builds concurrently
format: run clang-format only on touched C++ source/header files; user may request all C++ source/header files
clang-tidy: disabled unless explicitly requested
db integration tests need PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres
!mod test/bench: ONLY on func rename; broader api change->new test/bench
even if identified issue pre-existing fix it
after significant changes: commit before final response unless user says not to
benchmarks: report best, p10, p50 and p99, med as percentage diff over base... best + P10 also report +/- ns/iter (for scale)
