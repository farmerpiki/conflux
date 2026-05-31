if(CONFLUX_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

if(CONFLUX_BUILD_TESTS)
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
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                ${CF_TARGET}
                ${CF_EXPECT})
    set_tests_properties(${CF_TEST} PROPERTIES
        LABELS "${CF_LABELS}"
        RUN_SERIAL TRUE)
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

if(TARGET conflux_core)
    add_executable(conflux_api_surface_core_import_smoke
        api_surface_core_import_smoke.cxx)
    target_link_libraries(conflux_api_surface_core_import_smoke
        PRIVATE conflux_core conflux_options)
    add_test(NAME api-surface/import-core
        COMMAND conflux_api_surface_core_import_smoke)
    set_tests_properties(api-surface/import-core PROPERTIES
        LABELS "build;api-surface;modules"
        RUN_SERIAL TRUE)
endif()

if(TARGET conflux)
    foreach(_surface IN ITEMS curated extended complete selected)
        add_executable(conflux_api_surface_${_surface}_import_smoke
            api_surface_${_surface}_import_smoke.cxx)
        target_link_libraries(conflux_api_surface_${_surface}_import_smoke
            PRIVATE conflux conflux_options)
        add_test(NAME api-surface/import-${_surface}
            COMMAND conflux_api_surface_${_surface}_import_smoke)
        set_tests_properties(api-surface/import-${_surface} PROPERTIES
            LABELS "build;api-surface;modules"
            RUN_SERIAL TRUE)
    endforeach()

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_curated_compile_fail_workpool
        SOURCE api_surface_curated_compile_fail_workpool.cxx
        TEST api-surface/curated-hides-workpool
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux_api_surface_curated_unexpected_workpool_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_curated_compile_fail_iouring
        SOURCE api_surface_curated_compile_fail_iouring.cxx
        TEST api-surface/curated-hides-iouring
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::uring")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_curated_compile_fail_file
        SOURCE api_surface_curated_compile_fail_file.cxx
        TEST api-surface/curated-hides-blocking-file
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::http" "file")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_extended_compile_fail_iouring
        SOURCE api_surface_extended_compile_fail_iouring.cxx
        TEST api-surface/extended-hides-iouring
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::uring")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_complete_compile_fail_direct_slot_pool
        SOURCE api_surface_complete_compile_fail_direct_slot_pool.cxx
        TEST api-surface/complete-hides-direct-slot-pool
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "DirectSlotPool")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_detail
        SOURCE crypto_compile_fail_detail.cxx
        TEST api-surface/crypto-hides-detail
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::crypto::detail")
endif()

add_executable(conflux_tests)
set_source_files_properties(
    http_e2e_middleware.cxx
    http_e2e_observability.cxx
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(conflux_tests
    PRIVATE
        http_e2e.cxx
        http_e2e_middleware.cxx
        http_e2e_observability.cxx
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
if(ZLIB_FOUND)
    target_link_libraries(conflux_tests PRIVATE ZLIB::ZLIB)
endif()

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

if(CONFLUX_ENABLE_THIRD_PARTY_TESTS AND TARGET conflux_http_app)
    add_executable(conflux_third_party_conformance_server third_party_conformance_server.cxx)
    target_link_libraries(conflux_third_party_conformance_server PRIVATE conflux conflux_options)

    if(TARGET conflux_http2 AND CONFLUX_HAS_TLS STREQUAL "true")
        add_test(NAME third-party/h2spec
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/third-party/run-h2spec.sh"
                    "$<TARGET_FILE:conflux_third_party_conformance_server>")
        set_tests_properties(third-party/h2spec PROPERTIES
            LABELS "third-party;h2;conformance"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 120)
    endif()

    if(TARGET conflux_http_realtime)
        add_test(NAME third-party/autobahn
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/third-party/run-autobahn.sh"
                    "$<TARGET_FILE:conflux_third_party_conformance_server>")
        set_tests_properties(third-party/autobahn PROPERTIES
            LABELS "third-party;websocket;conformance"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 600)
    endif()
endif()

add_executable(conflux_crypto_tests crypto_test.cxx)
target_link_libraries(conflux_crypto_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(_conflux_cpu_feature_probes_runtime AND CONFLUX_HAS_AESNI)
    add_test(NAME simd/crypto-aesni-runtime-scalar-fallback
        COMMAND conflux_crypto_tests "crypto: aes_gcm empty plaintext decrypt")
    set_tests_properties(simd/crypto-aesni-runtime-scalar-fallback PROPERTIES
        ENVIRONMENT "CONFLUX_TEST_CPU_FEATURES_DISABLE=aesni_pclmul_sse41"
        LABELS "simd;crypto"
        RUN_SERIAL TRUE)
endif()

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

if(_conflux_cpu_feature_probes_runtime AND CONFLUX_HAS_AESNI)
    add_test(NAME simd/hex-ssse3-runtime-scalar-fallback
        COMMAND conflux_utils_tests "utils: hex_encode known values")
    set_tests_properties(simd/hex-ssse3-runtime-scalar-fallback PROPERTIES
        ENVIRONMENT "CONFLUX_TEST_CPU_FEATURES_DISABLE=aesni_pclmul_sse41"
        LABELS "simd;crypto"
        RUN_SERIAL TRUE)
endif()

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_jwt_tests jwt_test.cxx)
    target_link_libraries(conflux_jwt_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)
endif()

add_executable(conflux_password_hash_tests password_hash_test.cxx)
target_link_libraries(conflux_password_hash_tests PRIVATE conflux_http_auth conflux_options Catch2::Catch2WithMain)

if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_auth_secret_config_tests auth_secret_config_test.cxx)
    target_link_libraries(conflux_auth_secret_config_tests PRIVATE conflux_http_auth conflux_net_config conflux_options Catch2::Catch2WithMain)
endif()

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

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_config_compile_fail_global_build_uring_flags
        SOURCE http_server_config_compile_fail_global_build_uring_flags.cxx
        TEST http-server-config/compile-fail-global-build-uring-flags
        LINK conflux_http_server_config conflux_options
        LABELS http compile-fail
        EXPECT "build_uring_flags")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_config_compile_fail_global_flags_str
        SOURCE http_server_config_compile_fail_global_flags_str.cxx
        TEST http-server-config/compile-fail-global-flags-str
        LINK conflux_http_server_config conflux_options
        LABELS http compile-fail
        EXPECT "flags_str")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_config_compile_fail_global_wq_fd_for_ring
        SOURCE http_server_config_compile_fail_global_wq_fd_for_ring.cxx
        TEST http-server-config/compile-fail-global-wq-fd-for-ring
        LINK conflux_http_server_config conflux_options
        LABELS http compile-fail
        EXPECT "wq_fd_for_ring")
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

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_request
        SOURCE http_core_compile_fail_global_request.cxx
        TEST http-core/compile-fail-global-request
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "Request")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_request_view
        SOURCE http_core_compile_fail_global_request_view.cxx
        TEST http-core/compile-fail-global-request-view
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "RequestView")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_field_eq
        SOURCE http_core_compile_fail_global_field_eq.cxx
        TEST http-core/compile-fail-global-field-eq
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "FieldEq")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_field_hash
        SOURCE http_core_compile_fail_global_field_hash.cxx
        TEST http-core/compile-fail-global-field-hash
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "FieldHash")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_http_fields
        SOURCE http_core_compile_fail_global_http_fields.cxx
        TEST http-core/compile-fail-global-http-fields
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "HttpFields")

    conflux_add_compile_fail_test(
        TARGET conflux_http_core_compile_fail_global_http_fields_view
        SOURCE http_core_compile_fail_global_http_fields_view.cxx
        TEST http-core/compile-fail-global-http-fields-view
        LINK conflux_http_core conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldsView")
endif()

if(TARGET conflux_net_http)
    add_executable(conflux_http_facade_tests http_facade_test.cxx)
    target_link_libraries(conflux_http_facade_tests
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)

    add_executable(conflux_http_facade_import_smoke http_facade_import_smoke.cxx)
    target_link_libraries(conflux_http_facade_import_smoke
        PRIVATE conflux_net_http conflux_options Catch2::Catch2WithMain)

    add_library(conflux_http_facade_api_snapshot OBJECT http_facade_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_api_snapshot
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_extended_api_snapshot OBJECT http_facade_extended_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_extended_api_snapshot
        PRIVATE conflux_net_http conflux_options)

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_raw_string
        SOURCE http_facade_compile_fail_raw_string.cxx
        TEST http-facade/compile-fail-raw-string
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HTTP app handlers must not return raw strings" "use http::text(...)" "http::html(...)")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_raw_string_extractor
        SOURCE http_facade_compile_fail_raw_string_extractor.cxx
        TEST http-facade/compile-fail-raw-string-extractor
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HTTP app handlers must not return raw strings" "use http::text(...)" "http::html(...)")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_json_codec
        SOURCE http_facade_compile_fail_json_codec.cxx
        TEST http-facade/compile-fail-json-codec
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "http::Json<T> responses require T to be serializable"
               "add conflux::json::JsonCodec<T>, conflux::json::JsonMembers<T>, or reflection JSON support")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_legacy_bearer
        SOURCE http_facade_compile_fail_legacy_bearer.cxx
        TEST http-facade/compile-fail-legacy-bearer
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequiredBearer")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_cookie_builder
        SOURCE http_facade_compile_fail_global_cookie_builder.cxx
        TEST http-facade/compile-fail-global-cookie-builder
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "CookieBuilder")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_streamed_file
        SOURCE http_facade_compile_fail_global_streamed_file.cxx
        TEST http-facade/compile-fail-global-streamed-file
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StreamedFile")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_response
        SOURCE http_facade_compile_fail_global_response.cxx
        TEST http-facade/compile-fail-global-response
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Response")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_uploaded_file
        SOURCE http_facade_compile_fail_global_uploaded_file.cxx
        TEST http-facade/compile-fail-global-uploaded-file
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "UploadedFile")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_cloneable_function
        SOURCE http_facade_compile_fail_global_cloneable_function.cxx
        TEST http-facade/compile-fail-global-cloneable-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "CloneableFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_method
        SOURCE http_facade_compile_fail_global_http_method.cxx
        TEST http-facade/compile-fail-global-http-method
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpMethod")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_request_context
        SOURCE http_facade_compile_fail_global_request_context.cxx
        TEST http-facade/compile-fail-global-request-context
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequestContext")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_run_status
        SOURCE http_facade_compile_fail_global_run_status.cxx
        TEST http-facade/compile-fail-global-run-status
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RunStatus")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_config
        SOURCE http_facade_compile_fail_global_config.cxx
        TEST http-facade/compile-fail-global-config
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_parser_limits
        SOURCE http_facade_compile_fail_global_parser_limits.cxx
        TEST http-facade/compile-fail-global-parser-limits
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ParserLimits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_config_issue
        SOURCE http_facade_compile_fail_global_config_issue.cxx
        TEST http-facade/compile-fail-global-config-issue
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ConfigIssue")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_drain_options
        SOURCE http_facade_compile_fail_global_drain_options.cxx
        TEST http-facade/compile-fail-global-drain-options
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "DrainOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_drain_report
        SOURCE http_facade_compile_fail_global_drain_report.cxx
        TEST http-facade/compile-fail-global-drain-report
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "DrainReport")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_drain_stream_policy
        SOURCE http_facade_compile_fail_global_drain_stream_policy.cxx
        TEST http-facade/compile-fail-global-drain-stream-policy
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "DrainStreamPolicy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_overflow_policy
        SOURCE http_facade_compile_fail_global_overflow_policy.cxx
        TEST http-facade/compile-fail-global-overflow-policy
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "OverflowPolicy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_metrics
        SOURCE http_facade_compile_fail_global_send_zc_metrics.cxx
        TEST http-facade/compile-fail-global-send-zc-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_pending_action
        SOURCE http_facade_compile_fail_global_send_zc_pending_action.cxx
        TEST http-facade/compile-fail-global-send-zc-pending-action
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcPendingAction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_action
        SOURCE http_facade_compile_fail_global_send_zc_cqe_action.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-action
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeAction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_state
        SOURCE http_facade_compile_fail_global_send_zc_cqe_state.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-state
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeState")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_input
        SOURCE http_facade_compile_fail_global_send_zc_cqe_input.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-input
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeInput")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_send_zc_cqe_outcome
        SOURCE http_facade_compile_fail_global_send_zc_cqe_outcome.cxx
        TEST http-facade/compile-fail-global-send-zc-cqe-outcome
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SendZcCqeOutcome")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_field_source
        SOURCE http_facade_compile_fail_global_http_field_source.cxx
        TEST http-facade/compile-fail-global-http-field-source
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldSource")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_field_error_kind
        SOURCE http_facade_compile_fail_global_http_field_error_kind.cxx
        TEST http-facade/compile-fail-global-http-field-error-kind
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldErrorKind")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_field_error
        SOURCE http_facade_compile_fail_global_http_field_error.cxx
        TEST http-facade/compile-fail-global-http-field-error
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpFieldError")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_reject_reason
        SOURCE http_facade_compile_fail_global_http_reject_reason.cxx
        TEST http-facade/compile-fail-global-http-reject-reason
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpRejectReason")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_rejection_metrics
        SOURCE http_facade_compile_fail_global_http_rejection_metrics.cxx
        TEST http-facade/compile-fail-global-http-rejection-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpRejectionMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_pressure_metrics
        SOURCE http_facade_compile_fail_global_http_pressure_metrics.cxx
        TEST http-facade/compile-fail-global-http-pressure-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpPressureMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_server_metrics
        SOURCE http_facade_compile_fail_global_http_server_metrics.cxx
        TEST http-facade/compile-fail-global-http-server-metrics
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpServerMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_http_server_observability_hooks
        SOURCE http_facade_compile_fail_global_http_server_observability_hooks.cxx
        TEST http-facade/compile-fail-global-http-server-observability-hooks
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HttpServerObservabilityHooks")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_sse_channel
        SOURCE http_facade_compile_fail_global_sse_channel.cxx
        TEST http-facade/compile-fail-global-sse-channel
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SseChannel")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_sse_overflow_policy
        SOURCE http_facade_compile_fail_global_sse_overflow_policy.cxx
        TEST http-facade/compile-fail-global-sse-overflow-policy
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "SseOverflowPolicy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_ws_conn
        SOURCE http_facade_compile_fail_global_ws_conn.cxx
        TEST http-facade/compile-fail-global-ws-conn
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "WsConn")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_ws_upgrade
        SOURCE http_facade_compile_fail_global_ws_upgrade.cxx
        TEST http-facade/compile-fail-global-ws-upgrade
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "WsUpgrade")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_global_ws_detail
        SOURCE http_facade_compile_fail_global_ws_detail.cxx
        TEST http-facade/compile-fail-global-ws-detail
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ws_detail")

    conflux_add_compile_fail_test(
        TARGET conflux_json_compile_fail_global_customization
        SOURCE json_compile_fail_global_customization.cxx
        TEST json/compile-fail-global-customization
        LINK conflux_json conflux_options
        LABELS json compile-fail
        EXPECT "JsonMembers")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_router_alias
        SOURCE http_facade_compile_fail_router_alias.cxx
        TEST http-facade/compile-fail-router-alias
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_router_alias_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_route_policy_internal
        SOURCE http_facade_compile_fail_route_policy_internal.cxx
        TEST http-facade/compile-fail-route-policy-internal
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "AppRouteRateLimit")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_middleware_concept_alias
        SOURCE http_facade_compile_fail_middleware_concept_alias.cxx
        TEST http-facade/compile-fail-middleware-concept-alias
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_use_async_member
        SOURCE http_facade_compile_fail_use_async_member.cxx
        TEST http-facade/compile-fail-use-async-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "use_async" "conflux::http::App")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_use_async_helper
        SOURCE http_facade_compile_fail_use_async_helper.cxx
        TEST http-facade/compile-fail-use-async-helper
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "use_async")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_auth_policy_member
        SOURCE http_facade_compile_fail_auth_policy_member.cxx
        TEST http-facade/compile-fail-auth-policy-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "auth_policy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_openapi_handler_member
        SOURCE http_facade_compile_fail_openapi_handler_member.cxx
        TEST http-facade/compile-fail-openapi-handler-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_openapi_handler_member_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_router_member
        SOURCE http_facade_compile_fail_router_member.cxx
        TEST http-facade/compile-fail-router-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_router_member_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_route_infos_member
        SOURCE http_facade_compile_fail_route_infos_member.cxx
        TEST http-facade/compile-fail-route-infos-member
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_facade_unexpected_route_infos_member_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_offload_free
        SOURCE http_facade_compile_fail_offload_free.cxx
        TEST http-facade/compile-fail-offload-free
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "offload")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_openapi_handler_free
        SOURCE http_facade_compile_fail_openapi_handler_free.cxx
        TEST http-facade/compile-fail-openapi-handler-free
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "openapi_handler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_openapi_compile_fail_global_openapi_handler
        SOURCE http_openapi_compile_fail_global_openapi_handler.cxx
        TEST http-openapi/compile-fail-global-openapi-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "openapi_handler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_compile_fail_file_free
        SOURCE http_facade_compile_fail_file_free.cxx
        TEST http-facade/compile-fail-file-free
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "file")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_extended_compile_fail_global_workpool
        SOURCE http_facade_extended_compile_fail_global_workpool.cxx
        TEST http-facade-extended/compile-fail-global-workpool
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "conflux_http_extended_unexpected_workpool_global_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_extended_compile_fail_global_router
        SOURCE http_facade_extended_compile_fail_global_router.cxx
        TEST http-facade-extended/compile-fail-global-router
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Router")

    conflux_add_compile_fail_test(
        TARGET conflux_http_facade_extended_compile_fail_global_route_info
        SOURCE http_facade_extended_compile_fail_global_route_info.cxx
        TEST http-facade-extended/compile-fail-global-route-info
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RouteInfo")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_route_info
        SOURCE http_router_compile_fail_global_route_info.cxx
        TEST http-router/compile-fail-global-route-info
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RouteInfo")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_router
        SOURCE http_router_compile_fail_global_router.cxx
        TEST http-router/compile-fail-global-router
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Router")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_handler_result
        SOURCE http_router_compile_fail_global_handler_result.cxx
        TEST http-router/compile-fail-global-handler-result
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "HandlerResult")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_view_handler
        SOURCE http_router_compile_fail_global_view_handler.cxx
        TEST http-router/compile-fail-global-view-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ViewHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_request_handler
        SOURCE http_router_compile_fail_global_request_handler.cxx
        TEST http-router/compile-fail-global-request-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequestHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_route_handler
        SOURCE http_router_compile_fail_global_route_handler.cxx
        TEST http-router/compile-fail-global-route-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RouteHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_context_handler_function
        SOURCE http_router_compile_fail_global_context_handler_function.cxx
        TEST http-router/compile-fail-global-context-handler-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ContextHandlerFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_context_middleware_function
        SOURCE http_router_compile_fail_global_context_middleware_function.cxx
        TEST http-router/compile-fail-global-context-middleware-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ContextMiddlewareFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_async_middleware
        SOURCE http_router_compile_fail_global_async_middleware.cxx
        TEST http-router/compile-fail-global-async-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "AsyncMiddleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_view_middleware
        SOURCE http_router_compile_fail_global_view_middleware.cxx
        TEST http-router/compile-fail-global-view-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ViewMiddleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_request_middleware
        SOURCE http_router_compile_fail_global_request_middleware.cxx
        TEST http-router/compile-fail-global-request-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "RequestMiddleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_middleware
        SOURCE http_router_compile_fail_global_middleware.cxx
        TEST http-router/compile-fail-global-middleware
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "Middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_next_handler
        SOURCE http_router_compile_fail_global_next_handler.cxx
        TEST http-router/compile-fail-global-next-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "NextHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_middleware_function
        SOURCE http_router_compile_fail_global_middleware_function.cxx
        TEST http-router/compile-fail-global-middleware-function
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "MiddlewareFunction")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_compile_fail_global_context_next_handler
        SOURCE http_router_compile_fail_global_context_next_handler.cxx
        TEST http-router/compile-fail-global-context-next-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "ContextNextHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_static_compile_fail_global_static_route_handler
        SOURCE http_router_static_compile_fail_global_static_route_handler.cxx
        TEST http-router-static/compile-fail-global-static-route-handler
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StaticRouteHandler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_static_compile_fail_global_static_route_registration
        SOURCE http_router_static_compile_fail_global_static_route_registration.cxx
        TEST http-router-static/compile-fail-global-static-route-registration
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StaticRouteRegistration")

    conflux_add_compile_fail_test(
        TARGET conflux_http_static_compile_fail_global_static_options
        SOURCE http_static_compile_fail_global_static_options.cxx
        TEST http-static/compile-fail-global-static-options
        LINK conflux_net_http conflux_options
        LABELS http compile-fail
        EXPECT "StaticOptions")
endif()

if(TARGET conflux_http_policy)
    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_cache_control_options
        SOURCE http_policy_compile_fail_global_cache_control_options.cxx
        TEST http-policy/compile-fail-global-cache-control-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "CacheControlOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_forwarded_options
        SOURCE http_policy_compile_fail_global_forwarded_options.cxx
        TEST http-policy/compile-fail-global-forwarded-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "ForwardedOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_ip_filter_options
        SOURCE http_policy_compile_fail_global_ip_filter_options.cxx
        TEST http-policy/compile-fail-global-ip-filter-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "IpFilterOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_rate_limit_options
        SOURCE http_policy_compile_fail_global_rate_limit_options.cxx
        TEST http-policy/compile-fail-global-rate-limit-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "RateLimitOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_redirect_options
        SOURCE http_policy_compile_fail_global_redirect_options.cxx
        TEST http-policy/compile-fail-global-redirect-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "RedirectOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_response_cache_options
        SOURCE http_policy_compile_fail_global_response_cache_options.cxx
        TEST http-policy/compile-fail-global-response-cache-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "ResponseCacheOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_security_options
        SOURCE http_policy_compile_fail_global_security_options.cxx
        TEST http-policy/compile-fail-global-security-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "SecurityOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_policy_compile_fail_global_trailing_slash_options
        SOURCE http_policy_compile_fail_global_trailing_slash_options.cxx
        TEST http-policy/compile-fail-global-trailing-slash-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "TrailingSlashOptions")
endif()

if(TARGET conflux_http_observability)
    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_request_id_options
        SOURCE http_middleware_compile_fail_global_request_id_options.cxx
        TEST http-middleware/compile-fail-global-request-id-options
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "RequestIdOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_request_id_middleware
        SOURCE http_middleware_compile_fail_global_request_id_middleware.cxx
        TEST http-middleware/compile-fail-global-request-id-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "request_id_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_structured_log_options
        SOURCE http_middleware_compile_fail_global_structured_log_options.cxx
        TEST http-middleware/compile-fail-global-structured-log-options
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "StructuredLogOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_structured_log_middleware
        SOURCE http_middleware_compile_fail_global_structured_log_middleware.cxx
        TEST http-middleware/compile-fail-global-structured-log-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "structured_log_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_trace_context
        SOURCE http_middleware_compile_fail_global_trace_context.cxx
        TEST http-middleware/compile-fail-global-trace-context
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "TraceContext")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_tracing_context
        SOURCE http_middleware_compile_fail_global_tracing_context.cxx
        TEST http-middleware/compile-fail-global-tracing-context
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "TracingContext")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_tracing_options
        SOURCE http_middleware_compile_fail_global_tracing_options.cxx
        TEST http-middleware/compile-fail-global-tracing-options
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "TracingOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_tracing_middleware
        SOURCE http_middleware_compile_fail_global_tracing_middleware.cxx
        TEST http-middleware/compile-fail-global-tracing-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "tracing_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_counter
        SOURCE http_metrics_compile_fail_global_counter.cxx
        TEST http-metrics/compile-fail-global-counter
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "Counter")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_gauge
        SOURCE http_metrics_compile_fail_global_gauge.cxx
        TEST http-metrics/compile-fail-global-gauge
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "Gauge")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_histogram
        SOURCE http_metrics_compile_fail_global_histogram.cxx
        TEST http-metrics/compile-fail-global-histogram
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "Histogram")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_prometheus_label
        SOURCE http_metrics_compile_fail_global_prometheus_label.cxx
        TEST http-metrics/compile-fail-global-prometheus-label
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "PrometheusLabel")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_registry
        SOURCE http_metrics_compile_fail_global_metrics_registry.cxx
        TEST http-metrics/compile-fail-global-metrics-registry
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "MetricsRegistry")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_middleware
        SOURCE http_metrics_compile_fail_global_metrics_middleware.cxx
        TEST http-metrics/compile-fail-global-metrics-middleware
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "metrics_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_handler
        SOURCE http_metrics_compile_fail_global_metrics_handler.cxx
        TEST http-metrics/compile-fail-global-metrics-handler
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "metrics_handler")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_metrics_handler_protected
        SOURCE http_metrics_compile_fail_global_metrics_handler_protected.cxx
        TEST http-metrics/compile-fail-global-metrics-handler-protected
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "metrics_handler_protected")

    conflux_add_compile_fail_test(
        TARGET conflux_http_metrics_compile_fail_global_pressure_formatter
        SOURCE http_metrics_compile_fail_global_pressure_formatter.cxx
        TEST http-metrics/compile-fail-global-pressure-formatter
        LINK conflux_http_observability conflux_options
        LABELS http compile-fail
        EXPECT "format_pressure_metrics_prometheus")
endif()

if(TARGET conflux_http_auth)
    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_basic_auth_options
        SOURCE http_auth_compile_fail_global_basic_auth_options.cxx
        TEST http-auth/compile-fail-global-basic-auth-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "BasicAuthOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_basic_credentials
        SOURCE http_auth_compile_fail_global_basic_credentials.cxx
        TEST http-auth/compile-fail-global-basic-credentials
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "BasicCredentials")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_parse_basic_credentials
        SOURCE http_auth_compile_fail_global_parse_basic_credentials.cxx
        TEST http-auth/compile-fail-global-parse-basic-credentials
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "parse_basic_credentials")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_failure_limiter
        SOURCE http_auth_compile_fail_global_auth_failure_limiter.cxx
        TEST http-auth/compile-fail-global-auth-failure-limiter
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthFailureLimiter")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_options
        SOURCE http_auth_compile_fail_global_auth_throttle_options.cxx
        TEST http-auth/compile-fail-global-auth-throttle-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_outcome
        SOURCE http_auth_compile_fail_global_auth_throttle_outcome.cxx
        TEST http-auth/compile-fail-global-auth-throttle-outcome
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleOutcome")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_metrics
        SOURCE http_auth_compile_fail_global_auth_throttle_metrics.cxx
        TEST http-auth/compile-fail-global-auth-throttle-metrics
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleMetrics")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_middleware_options
        SOURCE http_auth_compile_fail_global_auth_throttle_middleware_options.cxx
        TEST http-auth/compile-fail-global-auth-throttle-middleware-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "AuthThrottleMiddlewareOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_key
        SOURCE http_auth_compile_fail_global_auth_throttle_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_remote_key
        SOURCE http_auth_compile_fail_global_auth_throttle_remote_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-remote-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_remote_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_form_key
        SOURCE http_auth_compile_fail_global_auth_throttle_form_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-form-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_form_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_query_key
        SOURCE http_auth_compile_fail_global_auth_throttle_query_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-query-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_query_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_bearer_key
        SOURCE http_auth_compile_fail_global_auth_throttle_bearer_key.cxx
        TEST http-auth/compile-fail-global-auth-throttle-bearer-key
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_bearer_key")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_too_many_requests
        SOURCE http_auth_compile_fail_global_auth_throttle_too_many_requests.cxx
        TEST http-auth/compile-fail-global-auth-throttle-too-many-requests
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_too_many_requests")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_auth_throttle_middleware
        SOURCE http_auth_compile_fail_global_auth_throttle_middleware.cxx
        TEST http-auth/compile-fail-global-auth-throttle-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "auth_throttle_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_basic_auth_middleware
        SOURCE http_auth_compile_fail_global_basic_auth_middleware.cxx
        TEST http-auth/compile-fail-global-basic-auth-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "basic_auth_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_bearer_auth_middleware
        SOURCE http_auth_compile_fail_global_bearer_auth_middleware.cxx
        TEST http-auth/compile-fail-global-bearer-auth-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "bearer_auth_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_claims
        SOURCE http_auth_compile_fail_global_jwt_claims.cxx
        TEST http-auth/compile-fail-global-jwt-claims
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "JwtClaims")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_options
        SOURCE http_auth_compile_fail_global_jwt_options.cxx
        TEST http-auth/compile-fail-global-jwt-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "JwtOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_options_from_config
        SOURCE http_auth_compile_fail_global_jwt_options_from_config.cxx
        TEST http-auth/compile-fail-global-jwt-options-from-config
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_options_from_config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_sign
        SOURCE http_auth_compile_fail_global_jwt_sign.cxx
        TEST http-auth/compile-fail-global-jwt-sign
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_sign")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_decode
        SOURCE http_auth_compile_fail_global_jwt_decode.cxx
        TEST http-auth/compile-fail-global-jwt-decode
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_decode")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_jwt_middleware
        SOURCE http_auth_compile_fail_global_jwt_middleware.cxx
        TEST http-auth/compile-fail-global-jwt-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "jwt_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_algorithm
        SOURCE http_auth_compile_fail_global_password_hash_algorithm.cxx
        TEST http-auth/compile-fail-global-password-hash-algorithm
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashAlgorithm")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_options
        SOURCE http_auth_compile_fail_global_password_hash_options.cxx
        TEST http-auth/compile-fail-global-password-hash-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_secrets
        SOURCE http_auth_compile_fail_global_password_hash_secrets.cxx
        TEST http-auth/compile-fail-global-password-hash-secrets
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashSecrets")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_resource_limits
        SOURCE http_auth_compile_fail_global_password_hash_resource_limits.cxx
        TEST http-auth/compile-fail-global-password-hash-resource-limits
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordHashResourceLimits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_verify_result
        SOURCE http_auth_compile_fail_global_password_verify_result.cxx
        TEST http-auth/compile-fail-global-password-verify-result
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "PasswordVerifyResult")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_argon2id_available
        SOURCE http_auth_compile_fail_global_password_hash_argon2id_available.cxx
        TEST http-auth/compile-fail-global-password-hash-argon2id-available
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_argon2id_available")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_configure_resource_limits
        SOURCE http_auth_compile_fail_global_password_hash_configure_resource_limits.cxx
        TEST http-auth/compile-fail-global-password-hash-configure-resource-limits
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_configure_resource_limits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_resource_limits_fn
        SOURCE http_auth_compile_fail_global_password_hash_resource_limits_fn.cxx
        TEST http-auth/compile-fail-global-password-hash-resource-limits-fn
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_resource_limits")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_pbkdf2_sha256_password_hash_options
        SOURCE http_auth_compile_fail_global_pbkdf2_sha256_password_hash_options.cxx
        TEST http-auth/compile-fail-global-pbkdf2-sha256-password-hash-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "pbkdf2_sha256_password_hash_options")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_secrets_from_config
        SOURCE http_auth_compile_fail_global_password_hash_secrets_from_config.cxx
        TEST http-auth/compile-fail-global-password-hash-secrets-from-config
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_secrets_from_config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash_with_salt
        SOURCE http_auth_compile_fail_global_password_hash_with_salt.cxx
        TEST http-auth/compile-fail-global-password-hash-with-salt
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash_with_salt")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_hash
        SOURCE http_auth_compile_fail_global_password_hash.cxx
        TEST http-auth/compile-fail-global-password-hash
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_hash")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_verify
        SOURCE http_auth_compile_fail_global_password_verify.cxx
        TEST http-auth/compile-fail-global-password-verify
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_verify")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_password_needs_rehash
        SOURCE http_auth_compile_fail_global_password_needs_rehash.cxx
        TEST http-auth/compile-fail-global-password-needs-rehash
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "password_needs_rehash")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_cookie_signing_options
        SOURCE http_auth_compile_fail_global_cookie_signing_options.cxx
        TEST http-auth/compile-fail-global-cookie-signing-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "CookieSigningOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_cookie_signing_middleware
        SOURCE http_auth_compile_fail_global_cookie_signing_middleware.cxx
        TEST http-auth/compile-fail-global-cookie-signing-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "cookie_signing_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_sign_cookie
        SOURCE http_auth_compile_fail_global_sign_cookie.cxx
        TEST http-auth/compile-fail-global-sign-cookie
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "sign_cookie")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_verify_cookie
        SOURCE http_auth_compile_fail_global_verify_cookie.cxx
        TEST http-auth/compile-fail-global-verify-cookie
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "verify_cookie")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_cookie_signing_options_from_config
        SOURCE http_auth_compile_fail_global_cookie_signing_options_from_config.cxx
        TEST http-auth/compile-fail-global-cookie-signing-options-from-config
        LINK conflux_http_auth conflux_net_config conflux_options
        LABELS http compile-fail
        EXPECT "cookie_signing_options_from_config")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_csrf_options
        SOURCE http_auth_compile_fail_global_csrf_options.cxx
        TEST http-auth/compile-fail-global-csrf-options
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "CsrfOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_auth_compile_fail_global_csrf_middleware
        SOURCE http_auth_compile_fail_global_csrf_middleware.cxx
        TEST http-auth/compile-fail-global-csrf-middleware
        LINK conflux_http_auth conflux_options
        LABELS http compile-fail
        EXPECT "csrf_middleware")
endif()

if(TARGET conflux_http_policy)
    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_cors_options
        SOURCE http_middleware_compile_fail_global_cors_options.cxx
        TEST http-middleware/compile-fail-global-cors-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "CorsOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_cors_default_max_age
        SOURCE http_middleware_compile_fail_global_cors_default_max_age.cxx
        TEST http-middleware/compile-fail-global-cors-default-max-age
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "kCorsDefaultMaxAge")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_cors_middleware
        SOURCE http_middleware_compile_fail_global_cors_middleware.cxx
        TEST http-middleware/compile-fail-global-cors-middleware
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "cors_middleware")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_etag_options
        SOURCE http_middleware_compile_fail_global_etag_options.cxx
        TEST http-middleware/compile-fail-global-etag-options
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "ETagOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_middleware_compile_fail_global_etag_middleware
        SOURCE http_middleware_compile_fail_global_etag_middleware.cxx
        TEST http-middleware/compile-fail-global-etag-middleware
        LINK conflux_http_policy conflux_options
        LABELS http compile-fail
        EXPECT "etag_middleware")

endif()

if(TARGET conflux_http_response)
    add_executable(conflux_http_response_tests http_response_test.cxx)
    target_link_libraries(conflux_http_response_tests
        PRIVATE conflux_http_response conflux_options Catch2::Catch2WithMain)

    conflux_add_compile_fail_test(
        TARGET conflux_http_response_compile_fail_global_deferred_response
        SOURCE http_response_compile_fail_global_deferred_response.cxx
        TEST http-response/compile-fail-global-deferred-response
        LINK conflux_http_response conflux_options
        LABELS http compile-fail
        EXPECT "DeferredResponse")

    conflux_add_compile_fail_test(
        TARGET conflux_http_response_compile_fail_global_response
        SOURCE http_response_compile_fail_global_response.cxx
        TEST http-response/compile-fail-global-response
        LINK conflux_http_response conflux_options
        LABELS http compile-fail
        EXPECT "Response")
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

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_helpers_compile_fail_global_format_response
        SOURCE http_server_helpers_compile_fail_global_format_response.cxx
        TEST http-server-helpers/compile-fail-global-format-response
        LINK conflux_http_server_helpers conflux_options
        LABELS http compile-fail
        EXPECT "format_response")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_helpers_compile_fail_global_expect_state
        SOURCE http_server_helpers_compile_fail_global_expect_state.cxx
        TEST http-server-helpers/compile-fail-global-expect-state
        LINK conflux_http_server_helpers conflux_options
        LABELS http compile-fail
        EXPECT "ExpectState")

    conflux_add_compile_fail_test(
        TARGET conflux_http_server_helpers_compile_fail_global_parse_cookies
        SOURCE http_server_helpers_compile_fail_global_parse_cookies.cxx
        TEST http-server-helpers/compile-fail-global-parse-cookies
        LINK conflux_http_server_helpers conflux_options
        LABELS http compile-fail
        EXPECT "parse_cookies")
endif()

if(TARGET conflux_http_parse_helpers)
    conflux_add_compile_fail_test(
        TARGET conflux_http_parse_helpers_compile_fail_global_chunk_state
        SOURCE http_parse_helpers_compile_fail_global_chunk_state.cxx
        TEST http-parse-helpers/compile-fail-global-chunk-state
        LINK conflux_http_parse_helpers conflux_options
        LABELS http compile-fail
        EXPECT "ChunkedDecodeState")

    conflux_add_compile_fail_test(
        TARGET conflux_http_parse_helpers_compile_fail_global_parse_urlencoded
        SOURCE http_parse_helpers_compile_fail_global_parse_urlencoded.cxx
        TEST http-parse-helpers/compile-fail-global-parse-urlencoded
        LINK conflux_http_parse_helpers conflux_options
        LABELS http compile-fail
        EXPECT "parse_urlencoded")

    conflux_add_compile_fail_test(
        TARGET conflux_http_parse_helpers_compile_fail_global_content_type
        SOURCE http_parse_helpers_compile_fail_global_content_type.cxx
        TEST http-parse-helpers/compile-fail-global-content-type
        LINK conflux_http_parse_helpers conflux_options
        LABELS http compile-fail
        EXPECT "content_type_media_type")
endif()

if(TARGET conflux_http_static_core)
    conflux_add_compile_fail_test(
        TARGET conflux_http_static_core_compile_fail_global_static_request
        SOURCE http_static_core_compile_fail_global_static_request.cxx
        TEST http-static-core/compile-fail-global-static-request
        LINK conflux_http_static_core conflux_options
        LABELS http compile-fail
        EXPECT "StaticRequest")

    conflux_add_compile_fail_test(
        TARGET conflux_http_static_core_compile_fail_global_static_cache_store
        SOURCE http_static_core_compile_fail_global_static_cache_store.cxx
        TEST http-static-core/compile-fail-global-static-cache-store
        LINK conflux_http_static_core conflux_options
        LABELS http compile-fail
        EXPECT "StaticCacheStore")

    conflux_add_compile_fail_test(
        TARGET conflux_http_static_core_compile_fail_global_normalize_static_path
        SOURCE http_static_core_compile_fail_global_normalize_static_path.cxx
        TEST http-static-core/compile-fail-global-normalize-static-path
        LINK conflux_http_static_core conflux_options
        LABELS http compile-fail
        EXPECT "normalize_static_path")
endif()

if(TARGET conflux_http_router_match)
    conflux_add_compile_fail_test(
        TARGET conflux_http_router_match_compile_fail_global_segment
        SOURCE http_router_match_compile_fail_global_segment.cxx
        TEST http-router-match/compile-fail-global-segment
        LINK conflux_http_router_match conflux_options
        LABELS http compile-fail
        EXPECT "Segment")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_match_compile_fail_global_parse_pattern
        SOURCE http_router_match_compile_fail_global_parse_pattern.cxx
        TEST http-router-match/compile-fail-global-parse-pattern
        LINK conflux_http_router_match conflux_options
        LABELS http compile-fail
        EXPECT "parse_pattern")
endif()

if(TARGET conflux_http_router_dispatch)
    conflux_add_compile_fail_test(
        TARGET conflux_http_router_dispatch_compile_fail_global_deferred_task_options
        SOURCE http_router_dispatch_compile_fail_global_deferred_task_options.cxx
        TEST http-router-dispatch/compile-fail-global-deferred-task-options
        LINK conflux_http_router_dispatch conflux_options
        LABELS http compile-fail
        EXPECT "DeferredTaskOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_dispatch_compile_fail_global_dispatch_sync_routes
        SOURCE http_router_dispatch_compile_fail_global_dispatch_sync_routes.cxx
        TEST http-router-dispatch/compile-fail-global-dispatch-sync-routes
        LINK conflux_http_router_dispatch conflux_options
        LABELS http compile-fail
        EXPECT "dispatch_sync_routes")

    conflux_add_compile_fail_test(
        TARGET conflux_http_router_dispatch_compile_fail_global_router_run_async_http_task
        SOURCE http_router_dispatch_compile_fail_global_router_run_async_http_task.cxx
        TEST http-router-dispatch/compile-fail-global-router-run-async-http-task
        LINK conflux_http_router_dispatch conflux_options
        LABELS http compile-fail
        EXPECT "router_run_async_http_task")
endif()

if(TARGET conflux_http_vhost)
    conflux_add_compile_fail_test(
        TARGET conflux_http_vhost_compile_fail_global_vhost_router
        SOURCE http_vhost_compile_fail_global_vhost_router.cxx
        TEST http-vhost/compile-fail-global-vhost-router
        LINK conflux_http_vhost conflux_options
        LABELS http compile-fail
        EXPECT "VHostRouter")
endif()

if(TARGET conflux_http_proxy)
    conflux_add_compile_fail_test(
        TARGET conflux_http_proxy_compile_fail_global_proxy_options
        SOURCE http_proxy_compile_fail_global_proxy_options.cxx
        TEST http-proxy/compile-fail-global-proxy-options
        LINK conflux_http_proxy conflux_options
        LABELS http compile-fail
        EXPECT "ProxyOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_proxy_compile_fail_global_blocking_proxy
        SOURCE http_proxy_compile_fail_global_blocking_proxy.cxx
        TEST http-proxy/compile-fail-global-blocking-proxy
        LINK conflux_http_proxy conflux_options
        LABELS http compile-fail
        EXPECT "blocking_proxy")

    conflux_add_compile_fail_test(
        TARGET conflux_http_proxy_compile_fail_global_async_proxy
        SOURCE http_proxy_compile_fail_global_async_proxy.cxx
        TEST http-proxy/compile-fail-global-async-proxy
        LINK conflux_http_proxy conflux_options
        LABELS http compile-fail
        EXPECT "async_proxy")
endif()

if(TARGET conflux_http_server)
    conflux_add_compile_fail_test(
        TARGET conflux_http_server_compile_fail_global_http_server
        SOURCE http_server_compile_fail_global_http_server.cxx
        TEST http-server/compile-fail-global-http-server
        LINK conflux_http_server conflux_options
        LABELS http compile-fail
        EXPECT "HttpServer")
endif()

if(TARGET conflux_http_compression)
    conflux_add_compile_fail_test(
        TARGET conflux_http_compression_compile_fail_global_compress_options
        SOURCE http_compression_compile_fail_global_compress_options.cxx
        TEST http-compression/compile-fail-global-compress-options
        LINK conflux_http_compression conflux_options
        LABELS http compile-fail
        EXPECT "CompressOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_http_compression_compile_fail_global_gzip_backend
        SOURCE http_compression_compile_fail_global_gzip_backend.cxx
        TEST http-compression/compile-fail-global-gzip-backend
        LINK conflux_http_compression conflux_options
        LABELS http compile-fail
        EXPECT "GzipBackend")

    conflux_add_compile_fail_test(
        TARGET conflux_http_compression_compile_fail_global_compress_middleware
        SOURCE http_compression_compile_fail_global_compress_middleware.cxx
        TEST http-compression/compile-fail-global-compress-middleware
        LINK conflux_http_compression conflux_options
        LABELS http compile-fail
        EXPECT "compress_middleware")
endif()

if(TARGET conflux_http_server AND CONFLUX_EXPERIMENTAL_SEND_ZC)
    add_executable(conflux_send_zc_lifecycle_tests send_zc_lifecycle_test.cxx)
    target_link_libraries(conflux_send_zc_lifecycle_tests
        PRIVATE conflux_http_server conflux_options Catch2::Catch2WithMain)
endif()

add_executable(conflux_template_tests template_test.cxx)
target_link_libraries(
    conflux_template_tests
    PRIVATE conflux_template conflux_json conflux_types conflux_options Catch2::Catch2WithMain)

add_executable(conflux_json_tests json_test.cxx)
target_link_libraries(conflux_json_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(_conflux_simd_selection STREQUAL "RUNTIME" AND CONFLUX_JSON_STDSIMD_IFUNC AND TARGET conflux_json)
    add_executable(conflux_json_simd_ifunc_test json_simd_ifunc_test.cxx)
    target_link_libraries(conflux_json_simd_ifunc_test PRIVATE conflux_json conflux_options)
    foreach(_conflux_json_simd_ifunc_variant IN ITEMS scalar sse2 avx2)
        add_test(NAME simd/json-ifunc-${_conflux_json_simd_ifunc_variant}
            COMMAND conflux_json_simd_ifunc_test ${_conflux_json_simd_ifunc_variant})
        set_tests_properties(simd/json-ifunc-${_conflux_json_simd_ifunc_variant} PROPERTIES
            ENVIRONMENT "CONFLUX_TEST_JSON_SIMD_IFUNC=${_conflux_json_simd_ifunc_variant}"
            LABELS "simd;json"
            RUN_SERIAL TRUE)
    endforeach()
endif()

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
    PRIVATE conflux_socket_io conflux_file_io_sync conflux_options conflux_direct_slot_pool Catch2::Catch2WithMain)
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

add_executable(conflux_work_race_tests work_race_test.cxx)
target_link_libraries(conflux_work_race_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

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

conflux_add_compile_fail_test(
    TARGET conflux_direct_slot_pool_compile_fail_global_pool
    SOURCE direct_slot_pool_compile_fail_global_pool.cxx
    TEST direct-slot-pool/compile-fail-global-pool
    LINK conflux_direct_slot_pool conflux_options
    LABELS compile-fail
    EXPECT "DirectSlotPool")

conflux_add_compile_fail_test(
    TARGET conflux_direct_slot_pool_compile_fail_global_state
    SOURCE direct_slot_pool_compile_fail_global_state.cxx
    TEST direct-slot-pool/compile-fail-global-state
    LINK conflux_direct_slot_pool conflux_options
    LABELS compile-fail
    EXPECT "DirectSlotState")

if(TARGET conflux_net_cancel)
    conflux_add_compile_fail_test(
        TARGET conflux_net_cancel_compile_fail_global_active_task_cancel_relay
        SOURCE net_cancel_compile_fail_global_active_task_cancel_relay.cxx
        TEST net-cancel/compile-fail-global-active-task-cancel-relay
        LINK conflux_net_cancel conflux_options
        LABELS compile-fail
        EXPECT "ActiveTaskCancelRelay")
endif()

if(TARGET conflux_net_io_buffer)
    conflux_add_compile_fail_test(
        TARGET conflux_net_io_buffer_compile_fail_global_io_buffer
        SOURCE net_io_buffer_compile_fail_global_io_buffer.cxx
        TEST net-io-buffer/compile-fail-global-io-buffer
        LINK conflux_net_io_buffer conflux_options
        LABELS compile-fail
        EXPECT "IoBuffer")

    conflux_add_compile_fail_test(
        TARGET conflux_net_io_buffer_compile_fail_global_io_plan
        SOURCE net_io_buffer_compile_fail_global_io_plan.cxx
        TEST net-io-buffer/compile-fail-global-io-plan
        LINK conflux_net_io_buffer conflux_options
        LABELS compile-fail
        EXPECT "IoPlan")
endif()

if(TARGET conflux_http2)
    conflux_add_compile_fail_test(
        TARGET conflux_http2_compile_fail_global_configure_alpn
        SOURCE http2_compile_fail_global_configure_alpn.cxx
        TEST http2/compile-fail-global-configure-alpn
        LINK conflux_http2 conflux_options
        LABELS http compile-fail
        EXPECT "http2_configure_alpn")

    conflux_add_compile_fail_test(
        TARGET conflux_http2_compile_fail_global_negotiated
        SOURCE http2_compile_fail_global_negotiated.cxx
        TEST http2/compile-fail-global-negotiated
        LINK conflux_http2 conflux_options
        LABELS http compile-fail
        EXPECT "http2_negotiated")
endif()

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

add_executable(conflux_client_cancellation_e2e client_cancellation_e2e.cxx)
target_link_libraries(conflux_client_cancellation_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

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

add_executable(conflux_proxy_hostile_upstream_e2e)
target_sources(conflux_proxy_hostile_upstream_e2e
    PRIVATE
        proxy_hostile_upstream_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_proxy_hostile_upstream_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_proxy_hostile_upstream_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_compression_matrix_e2e)
target_sources(conflux_compression_matrix_e2e
    PRIVATE
        compression_matrix_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_compression_matrix_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_compression_matrix_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)
if(ZLIB_FOUND)
    target_link_libraries(conflux_compression_matrix_e2e PRIVATE ZLIB::ZLIB)
endif()
if(TARGET PkgConfig::BROTLI)
    target_link_libraries(conflux_compression_matrix_e2e PRIVATE PkgConfig::BROTLI)
endif()

add_executable(conflux_chaos_resource_e2e)
target_sources(conflux_chaos_resource_e2e
    PRIVATE
        chaos_resource_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_chaos_resource_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_chaos_resource_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_observability_golden_e2e)
target_sources(conflux_observability_golden_e2e
    PRIVATE
        observability_golden_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_observability_golden_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_observability_golden_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_slowloris_e2e)
target_sources(conflux_http_slowloris_e2e
    PRIVATE
        http_slowloris_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_slowloris_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_slowloris_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_backpressure_e2e)
target_sources(conflux_http_backpressure_e2e
    PRIVATE
        http_backpressure_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_backpressure_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_backpressure_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_full_drain_contract_e2e)
target_sources(conflux_http_full_drain_contract_e2e
    PRIVATE
        http_full_drain_contract_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_full_drain_contract_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_full_drain_contract_e2e
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
if(TARGET conflux_jwt_tests)
    conflux_discover_tests(conflux_jwt_tests)
endif()
conflux_discover_tests(conflux_password_hash_tests)
if(TARGET conflux_auth_secret_config_tests)
    conflux_discover_tests(conflux_auth_secret_config_tests)
endif()
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
conflux_discover_tests(conflux_work_race_tests)
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
conflux_discover_tests(conflux_client_cancellation_e2e)
conflux_discover_tests(conflux_file_io_http_e2e)
conflux_discover_tests(conflux_proxy_hostile_upstream_e2e)
conflux_discover_tests(conflux_compression_matrix_e2e)
conflux_discover_tests(conflux_chaos_resource_e2e)
conflux_discover_tests(conflux_observability_golden_e2e)
conflux_discover_tests(conflux_http_slowloris_e2e)
conflux_discover_tests(conflux_http_backpressure_e2e)
conflux_discover_stress_tests(conflux_http_full_drain_contract_e2e)
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
endif()

if(CONFLUX_BUILD_TESTS)
    add_test(NAME build/no-std-streams
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_no_std_streams.py")
    set_tests_properties(build/no-std-streams PROPERTIES
        LABELS "build;lint"
    )

    add_test(NAME build/cmake-source-files
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-cmake-source-files.py")
    set_tests_properties(build/cmake-source-files PROPERTIES
        LABELS "build;lint"
    )

    add_test(NAME build/component-map
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-component-map.py")
    set_tests_properties(build/component-map PROPERTIES
        LABELS "build;docs;package"
    )

    add_test(NAME build/http-facade-snapshot
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-http-facade-snapshot.py")
    set_tests_properties(build/http-facade-snapshot PROPERTIES
        LABELS "build;http;lint"
    )

    add_test(NAME build/api-surface-map
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-api-surface-map.py")
    set_tests_properties(build/api-surface-map PROPERTIES
        LABELS "build;docs;public-api"
    )

    add_test(NAME build/package-config
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-package-config.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/package-config PROPERTIES
        LABELS "build;package"
    )

    add_test(NAME build/header-first-contact-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-header-first-contact-smoke.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/header-first-contact-smoke PROPERTIES
        LABELS "build;headers;package"
        RUN_SERIAL TRUE
        TIMEOUT 180
    )

    if(CONFLUX_RUN_HEADER_COMPONENT_SMOKE)
        add_test(NAME build/header-component-smoke
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-header-component-smoke.sh"
                    "${CMAKE_SOURCE_DIR}")
        set_tests_properties(build/header-component-smoke PROPERTIES
            LABELS "build;headers;package;expensive"
            RUN_SERIAL TRUE
            TIMEOUT 600
        )
    endif()

    add_test(NAME build/mixed-module-header-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-package-smoke-mixed-module-header.sh")
    set_tests_properties(build/mixed-module-header-smoke PROPERTIES
        LABELS "build;modules;headers;package"
        RUN_SERIAL TRUE
        TIMEOUT 1200
    )

    add_test(NAME build/public-module-import-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-public-module-import-smoke.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/public-module-import-smoke PROPERTIES
        LABELS "build;modules;package"
        RUN_SERIAL TRUE
        TIMEOUT 1200
    )

    if(TARGET conflux_simd_direct_shape)
        add_test(NAME build/simd-direct-shape
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --target conflux_simd_direct_shape --config "$<CONFIG>")
        set_tests_properties(build/simd-direct-shape PROPERTIES
            LABELS "build;simd"
            RUN_SERIAL TRUE
        )
    endif()

    add_test(NAME docs/planning-state
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-planning-state.py")
    set_tests_properties(docs/planning-state PROPERTIES
        LABELS "docs;lint"
    )

    add_test(NAME docs/release-docs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-release-docs.py")
    set_tests_properties(docs/release-docs PROPERTIES
        LABELS "docs;lint"
    )

    add_test(NAME docs/first-contact-public-dialect
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-first-contact-public-dialect.py")
    set_tests_properties(docs/first-contact-public-dialect PROPERTIES
        LABELS "docs;lint;public-api"
    )

    add_test(NAME docs/package-docs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-package-docs.py")
    set_tests_properties(docs/package-docs PROPERTIES
        LABELS "docs;package"
    )

    add_test(NAME docs/release-notes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-release-notes.py")
    set_tests_properties(docs/release-notes PROPERTIES
        LABELS "docs;release"
    )
endif()

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_PACKAGE_SMOKE_PREFIX)
    conflux_add_package_config_install_tree_test(
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/package-smoke")
endif()

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_RUN_INSTALL_TREE_SMOKE)
    conflux_add_install_tree_smoke_test("${CMAKE_SOURCE_DIR}")
endif()
