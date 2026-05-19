communication:clear+precise+succint
bench run->release
bench/examples/tests->!sandbox
sanitizer build/tests->!sandbox (Catch2 discovery executes test bins during build)
builds: never pass jobs/-j/--parallel; let Ninja/CMake auto-detect jobs; never run independent builds concurrently
format: run clang-format only on touched C++ source/header files; user may request all C++ source/header files
clang-tidy: disabled unless explicitly requested
db integration tests need PG_TEST_CONNINFO=postgresql:///conflux_test?user=postgres
!mod test/bench: ONLY on func rename; broader api change->new test/bench
even if identified issue pre-existing fix it
