find_package(Catch2 3 REQUIRED)
include(Catch)

set(CONFLUX_CATCH_RNG_SEED "" CACHE STRING "Optional Catch2 RNG seed used by discovered CTest tests")
set(CONFLUX_PG_TEST_CONNINFO "" CACHE STRING "libpq conninfo for DB integration tests (e.g. host=localhost user=postgres dbname=conflux_test)")

function(conflux_discover_tests target)
    set(extra_args --order lex)
    if(NOT CONFLUX_CATCH_RNG_SEED STREQUAL "")
        list(APPEND extra_args --rng-seed ${CONFLUX_CATCH_RNG_SEED})
    endif()
    catch_discover_tests(${target}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE
    )
endfunction()

function(conflux_discover_db_integration_tests target)
    set(extra_args --order lex)
    if(NOT CONFLUX_CATCH_RNG_SEED STREQUAL "")
        list(APPEND extra_args --rng-seed ${CONFLUX_CATCH_RNG_SEED})
    endif()
    set(env_props "")
    if(NOT CONFLUX_PG_TEST_CONNINFO STREQUAL "")
        list(APPEND env_props ENVIRONMENT "PG_TEST_CONNINFO=${CONFLUX_PG_TEST_CONNINFO}")
    endif()
    catch_discover_tests(${target}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE ${env_props}
    )
endfunction()

add_executable(conflux_tests)
target_sources(conflux_tests
    PRIVATE
        http_e2e.cxx
        http3_test.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
        smoke.cxx
)
target_include_directories(conflux_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_tests
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_tls_external)
    target_sources(conflux_tls_external
        PRIVATE
            tls_external.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            external_support.cxx
    )
    target_link_libraries(conflux_tls_external
        PRIVATE
            conflux
            conflux_options
            Catch2::Catch2WithMain
    )

    add_executable(conflux_smtp_tests smtp_test.cxx)
    target_link_libraries(conflux_smtp_tests
        PRIVATE conflux conflux_options Catch2::Catch2WithMain)
endif()

if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    add_executable(conflux_h2_external)
    target_sources(conflux_h2_external
        PRIVATE
            h2_external.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            external_support.cxx
    )
    target_link_libraries(conflux_h2_external
        PRIVATE conflux conflux_options Catch2::Catch2WithMain)
endif()

if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    add_executable(conflux_h3_external)
    target_sources(conflux_h3_external
        PRIVATE
            h3_external.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            external_support.cxx
    )
    target_link_libraries(conflux_h3_external
        PRIVATE conflux conflux_options Catch2::Catch2WithMain)
endif()

add_executable(conflux_crypto_tests crypto_test.cxx)
target_link_libraries(conflux_crypto_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

set(CONFLUX_BUILD_AES_GCM_COMPARE TRUE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC modules conflict/ICE when this standalone OpenSSL comparison TU includes
    # OpenSSL headers; the Catch2 crypto coverage above still builds and runs.
    set(CONFLUX_BUILD_AES_GCM_COMPARE FALSE)
endif()
if(CONFLUX_HAS_TLS STREQUAL "true" AND CONFLUX_BUILD_AES_GCM_COMPARE)
    add_executable(conflux_aes_gcm_compare aes_gcm_openssl_compare.cxx)
    target_link_libraries(conflux_aes_gcm_compare PRIVATE conflux conflux_options OpenSSL::SSL OpenSSL::Crypto)
endif()

add_executable(conflux_utils_tests utils_test.cxx)
target_link_libraries(conflux_utils_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_jwt_tests jwt_test.cxx)
target_link_libraries(conflux_jwt_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_template_tests template_test.cxx)
target_link_libraries(conflux_template_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_json_tests json_test.cxx)
target_link_libraries(conflux_json_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(CONFLUX_JSON_REFLECT)
    # Under -freflection (GCC 16 P2996), including Catch2 headers in any TU that
    # also imports modules compiled with -freflection causes pthreadtypes conflicts.
    # Use a standalone test module + thin main instead of Catch2.
    add_executable(conflux_json_reflect_tests)
    target_sources(conflux_json_reflect_tests
        PRIVATE
            json_reflection_main.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            json_reflection_test.cxx)
    target_link_libraries(conflux_json_reflect_tests
        PRIVATE conflux_types conflux_json conflux_json_reflect conflux_options)
    add_test(NAME conflux_json_reflect_tests COMMAND conflux_json_reflect_tests)
    set_tests_properties(conflux_json_reflect_tests PROPERTIES RUN_SERIAL TRUE)
endif()

add_executable(conflux_process_tests process_test.cxx)
target_link_libraries(conflux_process_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_resource_tests resource_limits_test.cxx)
target_link_libraries(conflux_resource_tests
    PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain PkgConfig::LIBURING)
conflux_discover_tests(conflux_resource_tests)

add_executable(conflux_json_conformance_external json_conformance_external.cxx)
target_link_libraries(conflux_json_conformance_external PRIVATE conflux conflux_options Catch2::Catch2WithMain)
conflux_discover_tests(conflux_json_conformance_external)

add_executable(conflux_json_testsuite_gate json_testsuite_gate.cxx)
target_link_libraries(conflux_json_testsuite_gate PRIVATE conflux conflux_options Catch2::Catch2WithMain)
target_compile_definitions(conflux_json_testsuite_gate PRIVATE "JSONTESTSUITE_DIR=\"${JSONTESTSUITE_DIR}\"")
conflux_discover_tests(conflux_json_testsuite_gate)

add_library(conflux_work_api_snapshot OBJECT work_api_snapshot.cxx)
target_link_libraries(conflux_work_api_snapshot PRIVATE conflux conflux_options)

add_executable(conflux_work_tests work_test.cxx)
target_link_libraries(conflux_work_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_root_tests work_root_test.cxx)
target_link_libraries(conflux_work_root_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_tests work_carrier_test.cxx)
target_link_libraries(conflux_work_carrier_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase2_tests work_carrier_phase2_test.cxx)
target_link_libraries(conflux_work_carrier_phase2_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase3_tests work_carrier_phase3_test.cxx)
target_link_libraries(conflux_work_carrier_phase3_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase4_tests work_carrier_phase4_test.cxx)
target_link_libraries(conflux_work_carrier_phase4_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase5_tests work_carrier_phase5_test.cxx)
target_link_libraries(conflux_work_carrier_phase5_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase6_tests work_carrier_phase6_test.cxx)
target_link_libraries(conflux_work_carrier_phase6_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_streams_tests work_carrier_streams_test.cxx)
target_link_libraries(conflux_work_carrier_streams_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_timer_tests work_carrier_timer_test.cxx)
target_link_libraries(conflux_work_carrier_timer_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_uring_flow_tests uring_flow_test.cxx)
target_link_libraries(conflux_uring_flow_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)
target_compile_definitions(conflux_uring_flow_tests PRIVATE CONFLUX_TESTING)

add_executable(conflux_file_io_tests file_io_test.cxx)
target_link_libraries(conflux_file_io_tests
    PRIVATE
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_dns_codec_tests dns_codec_test.cxx)
target_link_libraries(conflux_dns_codec_tests
    PRIVATE
        conflux_dns
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_dns_resolver_tests dns_resolver_test.cxx)
target_link_libraries(conflux_dns_resolver_tests
    PRIVATE
        conflux_dns
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
        PkgConfig::LIBURING
)

add_executable(conflux_udp_tests udp_test.cxx)
target_link_libraries(conflux_udp_tests
    PRIVATE
        conflux_socket_io
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

if(CONFLUX_HAS_DB STREQUAL "true")
    add_executable(conflux_db_tests db_test.cxx)
    target_link_libraries(conflux_db_tests
        PRIVATE
            conflux_db
            conflux_work
            conflux_file_io
            conflux_options
            Catch2::Catch2WithMain
            PkgConfig::LIBPQ
    )

    add_executable(conflux_db_integration db_integration_test.cxx)
    target_link_libraries(conflux_db_integration
        PRIVATE
            conflux_db
            conflux_work
            conflux_file_io
            conflux_options
            Catch2::Catch2WithMain
            PkgConfig::LIBPQ
            PkgConfig::LIBURING
    )
endif()

add_executable(conflux_file_io_http_e2e)
target_sources(conflux_file_io_http_e2e
    PRIVATE
        file_io_http_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_file_io_http_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_file_io_http_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
conflux_discover_tests(conflux_tests)
if(CONFLUX_HAS_TLS STREQUAL "true")
    conflux_discover_tests(conflux_tls_external)
endif()
conflux_discover_tests(conflux_crypto_tests)
conflux_discover_tests(conflux_utils_tests)
conflux_discover_tests(conflux_jwt_tests)
conflux_discover_tests(conflux_template_tests)
conflux_discover_tests(conflux_json_tests)
conflux_discover_tests(conflux_process_tests)
conflux_discover_tests(conflux_work_tests)
conflux_discover_tests(conflux_work_root_tests)
conflux_discover_tests(conflux_work_carrier_tests)
conflux_discover_tests(conflux_work_carrier_phase2_tests)
conflux_discover_tests(conflux_work_carrier_phase3_tests)
conflux_discover_tests(conflux_work_carrier_phase4_tests)
conflux_discover_tests(conflux_work_carrier_phase5_tests)
conflux_discover_tests(conflux_work_carrier_phase6_tests)
conflux_discover_tests(conflux_work_carrier_streams_tests)
conflux_discover_tests(conflux_work_carrier_timer_tests)
conflux_discover_tests(conflux_uring_flow_tests)
conflux_discover_tests(conflux_file_io_tests)
conflux_discover_tests(conflux_dns_codec_tests)
conflux_discover_tests(conflux_dns_resolver_tests)
conflux_discover_tests(conflux_udp_tests)
conflux_discover_tests(conflux_file_io_http_e2e)
if(CONFLUX_HAS_TLS STREQUAL "true")
    conflux_discover_tests(conflux_smtp_tests)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    conflux_discover_tests(conflux_h2_external)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    conflux_discover_tests(conflux_h3_external)
endif()
if(CONFLUX_HAS_DB STREQUAL "true")
    conflux_discover_tests(conflux_db_tests)
    conflux_discover_db_integration_tests(conflux_db_integration)
endif()
