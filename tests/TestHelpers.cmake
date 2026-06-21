find_package(Catch2 3 REQUIRED)
find_package(CURL QUIET)
include(Catch)

option(CONFLUX_BUILD_LIBCURL_EXTERNAL_TESTS
       "Build libcurl-based external HTTP tests"
       ${CURL_FOUND})

set(CONFLUX_CATCH_RNG_SEED "" CACHE STRING "Optional Catch2 RNG seed used by discovered CTest tests")

function(conflux_catch_extra_args out_var)
    set(extra_args --order lex)
    if(NOT CONFLUX_CATCH_RNG_SEED STREQUAL "")
        list(APPEND extra_args --rng-seed ${CONFLUX_CATCH_RNG_SEED})
    endif()
    set(${out_var} ${extra_args} PARENT_SCOPE)
endfunction()

function(conflux_discover_tests target)
    conflux_catch_extra_args(extra_args)
    catch_discover_tests(${target}
        ${ARGN}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE
        TIMEOUT 10
    )
endfunction()

function(conflux_discover_stress_tests target)
    conflux_catch_extra_args(extra_args)
    catch_discover_tests(${target}
        ${ARGN}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE LABELS stress TIMEOUT 120
    )
endfunction()

function(conflux_add_compile_fail_test)
    set(one_value_args TARGET SOURCE TEST)
    set(multi_value_args LINK LABELS EXPECT)
    cmake_parse_arguments(CF "" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT CF_TARGET OR NOT CF_SOURCE OR NOT CF_TEST OR NOT CF_LINK OR NOT CF_EXPECT)
        message(FATAL_ERROR "conflux_add_compile_fail_test requires TARGET, SOURCE, TEST, LINK, and EXPECT")
    endif()

    add_library(${CF_TARGET} EXCLUDE_FROM_ALL OBJECT ${CF_SOURCE})
    target_link_libraries(${CF_TARGET} PRIVATE ${CF_LINK})
    add_test(NAME ${CF_TEST}
        COMMAND "${PROJECT_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${PROJECT_BINARY_DIR}"
                ${CF_TARGET}
                ${CF_EXPECT})
    set_tests_properties(${CF_TEST} PROPERTIES
        LABELS "${CF_LABELS}"
        RUN_SERIAL TRUE)
endfunction()

function(conflux_discover_db_integration_tests target)
    conflux_catch_extra_args(extra_args)
    set(env_props "")
    if(NOT CONFLUX_PG_TEST_CONNINFO STREQUAL "")
        list(APPEND env_props ENVIRONMENT "PG_TEST_CONNINFO=${CONFLUX_PG_TEST_CONNINFO}")
    endif()
    catch_discover_tests(${target}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE LABELS "db;integration" SKIP_RETURN_CODE 4 ${env_props}
    )
endfunction()
