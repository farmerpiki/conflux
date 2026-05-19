find_package(Catch2 3 REQUIRED)
find_package(CURL QUIET)
include(Catch)

option(CONFLUX_BUILD_LIBCURL_EXTERNAL_TESTS
       "Build libcurl-based external HTTP tests"
       ${CURL_FOUND})

set(CONFLUX_CATCH_RNG_SEED "" CACHE STRING "Optional Catch2 RNG seed used by discovered CTest tests")
set(CONFLUX_PG_TEST_CONNINFO "" CACHE STRING "libpq conninfo for DB integration tests (e.g. host=localhost user=postgres dbname=conflux_test)")

function(conflux_discover_tests target)
    set(extra_args --order lex)
    if(NOT CONFLUX_CATCH_RNG_SEED STREQUAL "")
        list(APPEND extra_args --rng-seed ${CONFLUX_CATCH_RNG_SEED})
    endif()
    catch_discover_tests(${target}
        ${ARGN}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE
        TIMEOUT 10
    )
endfunction()

function(conflux_discover_stress_tests target)
    set(extra_args --order lex)
    if(NOT CONFLUX_CATCH_RNG_SEED STREQUAL "")
        list(APPEND extra_args --rng-seed ${CONFLUX_CATCH_RNG_SEED})
    endif()
    catch_discover_tests(${target}
        ${ARGN}
        EXTRA_ARGS ${extra_args}
        PROPERTIES RUN_SERIAL TRUE LABELS stress TIMEOUT 120
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
        PROPERTIES RUN_SERIAL TRUE LABELS "db;integration" SKIP_RETURN_CODE 4 ${env_props}
    )
endfunction()

add_test(NAME build/module-fragility-regression
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-module-interface-regressions.sh"
            "${CMAKE_SOURCE_DIR}")
set_tests_properties(build/module-fragility-regression PROPERTIES
    LABELS "build;modules"
    RUN_SERIAL TRUE
)

add_test(NAME build/optimized-presets
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-optimized-presets.sh"
            "${CMAKE_SOURCE_DIR}")
set_tests_properties(build/optimized-presets PROPERTIES
    LABELS "build;presets"
    RUN_SERIAL TRUE
)
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
        conflux_http_static_core
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

    if(CONFLUX_BUILD_LIBCURL_EXTERNAL_TESTS AND CURL_FOUND)
        add_executable(conflux_libcurl_external)
        target_sources(conflux_libcurl_external
            PRIVATE
                libcurl_external.cxx
            PRIVATE FILE_SET CXX_MODULES FILES
                external_support.cxx
        )
        target_link_libraries(conflux_libcurl_external
            PRIVATE
                conflux
                conflux_options
                Catch2::Catch2WithMain
                CURL::libcurl
        )

        add_executable(conflux_libcurl_external_stress)
        target_sources(conflux_libcurl_external_stress
            PRIVATE
                libcurl_external.cxx
            PRIVATE FILE_SET CXX_MODULES FILES
                external_support.cxx
        )
        target_compile_definitions(conflux_libcurl_external_stress
            PRIVATE CONFLUX_LIBCURL_STRESS_ONLY=1)
        target_link_libraries(conflux_libcurl_external_stress
            PRIVATE
                conflux
                conflux_options
                Catch2::Catch2WithMain
                CURL::libcurl
        )
    endif()
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

add_executable(conflux_password_hash_tests password_hash_test.cxx)
target_link_libraries(conflux_password_hash_tests PRIVATE conflux_http_auth conflux_options Catch2::Catch2WithMain)

add_executable(conflux_auth_secret_config_tests auth_secret_config_test.cxx)
target_link_libraries(conflux_auth_secret_config_tests PRIVATE conflux_http_auth conflux_net_config conflux_options Catch2::Catch2WithMain)

add_executable(conflux_auth_rate_limit_tests auth_rate_limit_test.cxx)
target_link_libraries(conflux_auth_rate_limit_tests PRIVATE conflux_http_auth conflux_options Catch2::Catch2WithMain)
if(TARGET conflux_http_server_config)
    add_executable(conflux_config_tests config_test.cxx)
    target_link_libraries(conflux_config_tests
        PRIVATE
            conflux_net_config
            conflux_uring
            conflux_http_server_config
            conflux_options
            Catch2::Catch2WithMain
    )
endif()

if(TARGET conflux_http_core)
    add_executable(http_request_assert_probe http_request_assert_probe.cxx)
    target_link_libraries(http_request_assert_probe
        PRIVATE conflux_http_core conflux_options)

    add_executable(conflux_http_core_tests)
    target_sources(conflux_http_core_tests
        PRIVATE
            http_core_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            assert_probe_support.cxx)
    target_link_libraries(conflux_http_core_tests
        PRIVATE conflux_http_core conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_http_core_tests PRIVATE
        "ASSERT_PROBE_BIN=\"$<TARGET_FILE:http_request_assert_probe>\"")
    add_dependencies(conflux_http_core_tests http_request_assert_probe)
endif()

if(TARGET conflux_net_http)
    add_executable(conflux_http_facade_tests http_facade_test.cxx)
    target_link_libraries(conflux_http_facade_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)

    add_executable(conflux_http_facade_import_smoke http_facade_import_smoke.cxx)
    target_link_libraries(conflux_http_facade_import_smoke
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)

    add_executable(conflux_http_facade_include_smoke http_facade_include_smoke.cxx)
    target_link_libraries(conflux_http_facade_include_smoke
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)

    add_library(conflux_http_facade_api_snapshot OBJECT http_facade_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_api_snapshot
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_request_view_task EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_request_view_task.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_request_view_task
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_raw_string EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_raw_string.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_raw_string
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_raw_string_extractor EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_raw_string_extractor.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_raw_string_extractor
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_json_codec EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_json_codec.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_json_codec
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_async_middleware EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_async_middleware.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_async_middleware
        PRIVATE conflux_net_http conflux_options)

    add_test(NAME http-facade/compile-fail-request-view-task
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_request_view_task
                "Async handlers must take HttpRequest const&, not HttpRequestView const&"
                "the view can dangle after coroutine suspension")
    set_tests_properties(http-facade/compile-fail-request-view-task PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-raw-string
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_raw_string
                "HTTP app handlers must not return raw strings"
                "use http::text(...)"
                "http::html(...)")
    set_tests_properties(http-facade/compile-fail-raw-string PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-raw-string-extractor
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_raw_string_extractor
                "HTTP app handlers must not return raw strings"
                "use http::text(...)"
                "http::html(...)")
    set_tests_properties(http-facade/compile-fail-raw-string-extractor PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-json-codec
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_json_codec
                "http::Json<T> responses require T to be serializable"
                "add JsonCodec<T>, JsonMembers<T>, or reflection JSON support")
    set_tests_properties(http-facade/compile-fail-json-codec PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-async-middleware
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_async_middleware
                "Async middleware must take http::Request const&"
                "RequestView const&")
    set_tests_properties(http-facade/compile-fail-async-middleware PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)
endif()

if(TARGET conflux_http_response)
    add_executable(conflux_http_response_tests http_response_test.cxx)
    target_link_libraries(conflux_http_response_tests
        PRIVATE conflux_http_response conflux_options Catch2::Catch2WithMain)
endif()

if(TARGET conflux_http_response_json AND TARGET conflux_http_app_json AND TARGET conflux_http_native_json)
    add_executable(conflux_http_json_tests http_json_test.cxx)
    target_link_libraries(conflux_http_json_tests
        PRIVATE conflux_http_app_json conflux_http_native_json conflux_options Catch2::Catch2WithMain)
endif()

if(TARGET conflux_http_server_helpers)
    add_executable(conflux_http_server_helpers_tests http_server_helpers_test.cxx)
    target_link_libraries(conflux_http_server_helpers_tests
        PRIVATE conflux_http_server_helpers conflux_http_realtime conflux_options Catch2::Catch2WithMain)
endif()

if(TARGET conflux_http_server AND CONFLUX_EXPERIMENTAL_SEND_ZC)
    add_executable(conflux_send_zc_lifecycle_tests send_zc_lifecycle_test.cxx)
    target_link_libraries(conflux_send_zc_lifecycle_tests
        PRIVATE conflux_http_server conflux_options Catch2::Catch2WithMain)
endif()

add_executable(conflux_template_tests template_test.cxx)
target_link_libraries(conflux_template_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_json_tests json_test.cxx)
target_link_libraries(conflux_json_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(TARGET conflux_json_native_provider)
    add_executable(conflux_json_boundary_tests json_boundary_test.cxx)
    target_link_libraries(conflux_json_boundary_tests
        PRIVATE conflux_json_native_provider conflux_options Catch2::Catch2WithMain)
endif()

if(TARGET conflux_json_file)
    add_executable(conflux_json_file_tests json_file_test.cxx)
    target_link_libraries(conflux_json_file_tests
        PRIVATE conflux_json_file conflux_file_io_sync conflux_options Catch2::Catch2WithMain)
endif()

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
        PRIVATE conflux_types conflux_json conflux_json_reflect conflux_json_reflect_provider conflux_options)
    target_compile_options(conflux_json_reflect_tests PRIVATE -freflection)
    add_test(NAME conflux_json_reflect_tests COMMAND conflux_json_reflect_tests)
    set_tests_properties(conflux_json_reflect_tests PROPERTIES RUN_SERIAL TRUE)
endif()

add_executable(conflux_process_tests process_test.cxx)
target_link_libraries(conflux_process_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_resource_tests resource_limits_test.cxx)
target_link_libraries(conflux_resource_tests
    PRIVATE conflux_socket_io conflux_options conflux_direct_slot_pool Catch2::Catch2WithMain)
conflux_discover_tests(conflux_resource_tests)

if(CONFLUX_ENABLE_RECV_BUNDLE)
    add_executable(recv_bundle_assert_probe recv_bundle_assert_probe.cxx)
    target_link_libraries(recv_bundle_assert_probe
        PRIVATE conflux_socket_io conflux_options)

    add_executable(conflux_recv_bundle_tests)
    target_sources(conflux_recv_bundle_tests
        PRIVATE
            recv_bundle_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            assert_probe_support.cxx)
    target_link_libraries(conflux_recv_bundle_tests
        PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_recv_bundle_tests PRIVATE
        "ASSERT_PROBE_BIN=\"$<TARGET_FILE:recv_bundle_assert_probe>\"")
    conflux_discover_tests(conflux_recv_bundle_tests)
endif()

if(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF)
    add_executable(incremental_buf_ring_assert_probe incremental_buf_ring_assert_probe.cxx)
    target_link_libraries(incremental_buf_ring_assert_probe
        PRIVATE conflux_socket_io conflux_options)

    add_executable(conflux_incremental_buf_ring_tests)
    target_sources(conflux_incremental_buf_ring_tests
        PRIVATE
            incremental_buf_ring_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            assert_probe_support.cxx)
    target_link_libraries(conflux_incremental_buf_ring_tests
        PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_incremental_buf_ring_tests PRIVATE
        "ASSERT_PROBE_BIN=\"$<TARGET_FILE:incremental_buf_ring_assert_probe>\"")
    conflux_discover_tests(conflux_incremental_buf_ring_tests)
endif()

add_executable(conflux_tcp_listener_tests tcp_listener_test.cxx)
target_link_libraries(conflux_tcp_listener_tests
    PRIVATE conflux_socket_io conflux_options Catch2::Catch2WithMain)
conflux_discover_tests(conflux_tcp_listener_tests)

if(CONFLUX_ENABLE_RECV_BUNDLE)
    add_executable(conflux_recv_bundle_e2e_tests)
    target_sources(conflux_recv_bundle_e2e_tests
        PRIVATE
            recv_bundle_e2e_test.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            support.cxx
    )
    target_link_libraries(conflux_recv_bundle_e2e_tests
        PRIVATE conflux conflux_options Catch2::Catch2WithMain)
    conflux_discover_tests(conflux_recv_bundle_e2e_tests)
endif()

add_executable(conflux_json_conformance_external json_conformance_external.cxx)
target_link_libraries(conflux_json_conformance_external PRIVATE conflux conflux_options Catch2::Catch2WithMain)
conflux_discover_tests(conflux_json_conformance_external)

if(CONFLUX_HAS_JSON_TESTSUITE)
    add_executable(conflux_json_testsuite_gate json_testsuite_gate.cxx)
    target_link_libraries(conflux_json_testsuite_gate PRIVATE conflux conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_json_testsuite_gate PRIVATE "JSONTESTSUITE_DIR=\"${JSONTESTSUITE_DIR}\"")
    conflux_discover_tests(conflux_json_testsuite_gate)
endif()

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

add_executable(conflux_owned_path_flow_tests owned_path_flow_test.cxx)
target_link_libraries(conflux_owned_path_flow_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)
target_compile_definitions(conflux_owned_path_flow_tests PRIVATE CONFLUX_TESTING)

add_executable(conflux_direct_slot_pool_tests direct_slot_pool_test.cxx)
target_link_libraries(conflux_direct_slot_pool_tests
    PRIVATE
        conflux_types
        conflux_options
        conflux_direct_slot_pool
        Catch2::Catch2WithMain
)
target_include_directories(conflux_direct_slot_pool_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
conflux_discover_tests(conflux_direct_slot_pool_tests)

add_executable(conflux_cq_overflow_tests cq_overflow_test.cxx)
target_link_libraries(conflux_cq_overflow_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)

if(CONFLUX_EXPERIMENTAL_RING_GROWTH)
    add_executable(conflux_ring_resize_tests ring_resize_test.cxx)
    target_link_libraries(conflux_ring_resize_tests
        PRIVATE
            conflux_uring
            conflux_options
            Catch2::Catch2WithMain
    )
endif()

add_executable(conflux_uring_caps_tests caps_test.cxx)
target_link_libraries(conflux_uring_caps_tests
    PRIVATE
        conflux_uring
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_poll_first_auto_tests poll_first_auto_test.cxx)
target_link_libraries(conflux_poll_first_auto_tests
    PRIVATE
        conflux_socket_io
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_direct_accept_sqe_tests direct_accept_sqe_test.cxx)
target_link_libraries(conflux_direct_accept_sqe_tests
    PRIVATE
        conflux_socket_io
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_tests file_io_test.cxx)
target_link_libraries(conflux_file_io_tests
    PRIVATE
        conflux_file_io
        conflux_work
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_file_io_sync_tests file_io_sync_test.cxx)
target_link_libraries(conflux_file_io_sync_tests
    PRIVATE
        conflux_file_io_sync
        conflux_file_map
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

add_executable(conflux_socket_task_ring_tests socket_task_ring_test.cxx)
target_link_libraries(conflux_socket_task_ring_tests
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

add_executable(conflux_http_overflow_stress_tests)
target_sources(conflux_http_overflow_stress_tests
    PRIVATE
        http_overflow_stress_test.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_overflow_stress_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_overflow_stress_tests
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
conflux_discover_tests(conflux_password_hash_tests)
conflux_discover_tests(conflux_auth_secret_config_tests)
conflux_discover_tests(conflux_auth_rate_limit_tests)
if(TARGET conflux_config_tests)
    conflux_discover_tests(conflux_config_tests)
endif()
if(TARGET conflux_http_core_tests)
    conflux_discover_tests(conflux_http_core_tests)
endif()
if(TARGET conflux_http_facade_tests)
    conflux_discover_tests(conflux_http_facade_tests)
endif()

if(TARGET conflux_http_facade_import_smoke)
    conflux_discover_tests(conflux_http_facade_import_smoke)
endif()
if(TARGET conflux_http_facade_include_smoke)
    conflux_discover_tests(conflux_http_facade_include_smoke)
endif()
if(TARGET conflux_http_response_tests)
    conflux_discover_tests(conflux_http_response_tests)
endif()
if(TARGET conflux_http_json_tests)
    conflux_discover_tests(conflux_http_json_tests)
endif()
if(TARGET conflux_http_server_helpers_tests)
    conflux_discover_tests(conflux_http_server_helpers_tests)
endif()
if(TARGET conflux_send_zc_lifecycle_tests)
    conflux_discover_tests(conflux_send_zc_lifecycle_tests)
endif()
conflux_discover_tests(conflux_template_tests)
conflux_discover_tests(conflux_json_tests)
if(TARGET conflux_json_file_tests)
    conflux_discover_tests(conflux_json_file_tests)
endif()
conflux_discover_tests(conflux_process_tests)
if(CONFLUX_ENABLE_TSAN)
    conflux_discover_tests(conflux_work_tests TEST_SPEC "~[stress]")
    conflux_discover_stress_tests(conflux_work_tests TEST_SPEC "[stress]")
else()
    conflux_discover_tests(conflux_work_tests)
endif()
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
conflux_discover_tests(conflux_owned_path_flow_tests)
conflux_discover_tests(conflux_cq_overflow_tests)
if(TARGET conflux_ring_resize_tests)
    conflux_discover_tests(conflux_ring_resize_tests)
endif()
conflux_discover_tests(conflux_uring_caps_tests)
conflux_discover_tests(conflux_poll_first_auto_tests)
conflux_discover_tests(conflux_direct_accept_sqe_tests)
conflux_discover_tests(conflux_file_io_tests)
conflux_discover_tests(conflux_file_io_sync_tests)
conflux_discover_tests(conflux_dns_codec_tests)
conflux_discover_tests(conflux_dns_resolver_tests)
conflux_discover_tests(conflux_udp_tests)
conflux_discover_tests(conflux_socket_task_ring_tests)
conflux_discover_tests(conflux_file_io_http_e2e)
conflux_discover_stress_tests(conflux_http_overflow_stress_tests)
if(CONFLUX_HAS_TLS STREQUAL "true")
    conflux_discover_tests(conflux_smtp_tests)
endif()
if(TARGET conflux_libcurl_external)
    conflux_discover_tests(conflux_libcurl_external)
endif()
if(TARGET conflux_libcurl_external_stress)
    conflux_discover_stress_tests(conflux_libcurl_external_stress)
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
