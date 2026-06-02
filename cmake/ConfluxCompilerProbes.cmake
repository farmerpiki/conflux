set(_CONFLUX_DISCARD_TEST_CODE "
int f();
int main() {
    auto _ = f();
    return 0;
}")
set(_conflux_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-Werror -Wpedantic")
check_cxx_source_compiles("${_CONFLUX_DISCARD_TEST_CODE}" CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD)
set(CMAKE_REQUIRED_FLAGS "${_conflux_saved_required_flags}")
unset(_conflux_saved_required_flags)
