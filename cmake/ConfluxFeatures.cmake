include(ConfluxProviderResolution)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE CONFLUX_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CONFLUX_GIT_COMMIT)
    set(CONFLUX_GIT_COMMIT "unknown")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CONFLUX_STDLIB_NAME "libstdc++")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CONFLUX_STDLIB_NAME "libc++/libstdc++")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CONFLUX_STDLIB_NAME "msvc-stl")
else()
    set(CONFLUX_STDLIB_NAME "unknown")
endif()
set(CONFLUX_STDLIB_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

# Generate the conflux.features module from the template.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux_features.cxx.in"
    "${CMAKE_CURRENT_BINARY_DIR}/src/conflux_features.cxx"
    @ONLY
)
