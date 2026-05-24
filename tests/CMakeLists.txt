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

    add_library(conflux_api_surface_curated_compile_fail_workpool EXCLUDE_FROM_ALL
        OBJECT api_surface_curated_compile_fail_workpool.cxx)
    target_link_libraries(conflux_api_surface_curated_compile_fail_workpool
        PRIVATE conflux conflux_options)
    add_test(NAME api-surface/curated-hides-workpool
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_api_surface_curated_compile_fail_workpool
                "WorkPool")
    set_tests_properties(api-surface/curated-hides-workpool PROPERTIES
        LABELS "build;api-surface;compile-fail"
        RUN_SERIAL TRUE)

    add_library(conflux_api_surface_curated_compile_fail_iouring EXCLUDE_FROM_ALL
        OBJECT api_surface_curated_compile_fail_iouring.cxx)
    target_link_libraries(conflux_api_surface_curated_compile_fail_iouring
        PRIVATE conflux conflux_options)
    add_test(NAME api-surface/curated-hides-iouring
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_api_surface_curated_compile_fail_iouring
                "IoUring")
    set_tests_properties(api-surface/curated-hides-iouring PROPERTIES
        LABELS "build;api-surface;compile-fail"
        RUN_SERIAL TRUE)

    add_library(conflux_api_surface_curated_compile_fail_file EXCLUDE_FROM_ALL
        OBJECT api_surface_curated_compile_fail_file.cxx)
    target_link_libraries(conflux_api_surface_curated_compile_fail_file
        PRIVATE conflux conflux_options)
    add_test(NAME api-surface/curated-hides-blocking-file
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_api_surface_curated_compile_fail_file
                "file")
    set_tests_properties(api-surface/curated-hides-blocking-file PROPERTIES
        LABELS "build;api-surface;compile-fail"
        RUN_SERIAL TRUE)

    add_library(conflux_api_surface_extended_compile_fail_iouring EXCLUDE_FROM_ALL
        OBJECT api_surface_extended_compile_fail_iouring.cxx)
    target_link_libraries(conflux_api_surface_extended_compile_fail_iouring
        PRIVATE conflux conflux_options)
    add_test(NAME api-surface/extended-hides-iouring
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_api_surface_extended_compile_fail_iouring
                "IoUring")
    set_tests_properties(api-surface/extended-hides-iouring PROPERTIES
        LABELS "build;api-surface;compile-fail"
        RUN_SERIAL TRUE)

    add_library(conflux_api_surface_complete_compile_fail_direct_slot_pool EXCLUDE_FROM_ALL
        OBJECT api_surface_complete_compile_fail_direct_slot_pool.cxx)
    target_link_libraries(conflux_api_surface_complete_compile_fail_direct_slot_pool
        PRIVATE conflux conflux_options)
    add_test(NAME api-surface/complete-hides-direct-slot-pool
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_api_surface_complete_compile_fail_direct_slot_pool
                "direct_slot_pool")
    set_tests_properties(api-surface/complete-hides-direct-slot-pool PROPERTIES
        LABELS "build;api-surface;compile-fail"
        RUN_SERIAL TRUE)
endif()

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

    add_library(conflux_http_facade_api_snapshot OBJECT http_facade_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_api_snapshot
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_extended_api_snapshot OBJECT http_facade_extended_api_snapshot.cxx)
    target_link_libraries(conflux_http_facade_extended_api_snapshot
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

    add_library(conflux_http_facade_compile_fail_router_alias EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_router_alias.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_router_alias
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_route_policy_internal EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_route_policy_internal.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_route_policy_internal
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_middleware_concept_alias EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_middleware_concept_alias.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_middleware_concept_alias
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_use_async_member EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_use_async_member.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_use_async_member
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_use_async_helper EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_use_async_helper.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_use_async_helper
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_openapi_handler_member EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_openapi_handler_member.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_openapi_handler_member
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_router_member EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_router_member.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_router_member
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_route_infos_member EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_route_infos_member.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_route_infos_member
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_offload_free EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_offload_free.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_offload_free
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_openapi_handler_free EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_openapi_handler_free.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_openapi_handler_free
        PRIVATE conflux_net_http conflux_options)

    add_library(conflux_http_facade_compile_fail_file_free EXCLUDE_FROM_ALL
        OBJECT http_facade_compile_fail_file_free.cxx)
    target_link_libraries(conflux_http_facade_compile_fail_file_free
        PRIVATE conflux_net_http conflux_options)

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

    add_test(NAME http-facade/compile-fail-router-alias
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_router_alias
                "Router"
                "conflux::http")
    set_tests_properties(http-facade/compile-fail-router-alias PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-route-policy-internal
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_route_policy_internal
                "AppRouteRateLimit"
                "conflux::http")
    set_tests_properties(http-facade/compile-fail-route-policy-internal PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-middleware-concept-alias
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_middleware_concept_alias
                "Middleware"
                "conflux::http")
    set_tests_properties(http-facade/compile-fail-middleware-concept-alias PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-use-async-member
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_use_async_member
                "use_async"
                "conflux::http::App")
    set_tests_properties(http-facade/compile-fail-use-async-member PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-use-async-helper
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_use_async_helper
                "use_async")
    set_tests_properties(http-facade/compile-fail-use-async-helper PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-openapi-handler-member
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_openapi_handler_member
                "openapi_handler"
                "conflux::http::App")
    set_tests_properties(http-facade/compile-fail-openapi-handler-member PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-router-member
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_router_member
                "router"
                "conflux::http::App")
    set_tests_properties(http-facade/compile-fail-router-member PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-route-infos-member
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_route_infos_member
                "route_infos"
                "conflux::http::App")
    set_tests_properties(http-facade/compile-fail-route-infos-member PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-offload-free
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_offload_free
                "no member named 'offload' in namespace 'conflux::http'")
    set_tests_properties(http-facade/compile-fail-offload-free PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-openapi-handler-free
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_openapi_handler_free
                "no member named 'openapi_handler' in namespace 'conflux::http'")
    set_tests_properties(http-facade/compile-fail-openapi-handler-free PROPERTIES
        LABELS "http;compile-fail"
        RUN_SERIAL TRUE)

    add_test(NAME http-facade/compile-fail-file-free
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-compile-fail-target.sh"
                "${CMAKE_BINARY_DIR}"
                conflux_http_facade_compile_fail_file_free
                "declaration of 'file' must be imported from module 'conflux.net.config'")
    set_tests_properties(http-facade/compile-fail-file-free PROPERTIES
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

    add_test(NAME build/header-component-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-header-component-smoke.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/header-component-smoke PROPERTIES
        LABELS "build;headers;package"
        RUN_SERIAL TRUE
        TIMEOUT 600
    )

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
    set(_conflux_package_smoke_args
        --source "${CMAKE_SOURCE_DIR}"
        --prefix "${CONFLUX_PACKAGE_SMOKE_PREFIX}"
        --build-dir "${CMAKE_BINARY_DIR}/package-smoke"
        --components "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}")
    if(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE)
        list(APPEND _conflux_package_smoke_args
            --interface-mode "${CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE}")
    endif()
    if(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER)
        list(APPEND _conflux_package_smoke_args --mixed-module-header)
    endif()
    add_test(NAME build/package-config-install-tree
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/run-package-config-smoke.sh"
                ${_conflux_package_smoke_args})
    set_tests_properties(build/package-config-install-tree PROPERTIES
        LABELS "build;package;install"
    )
endif()

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_RUN_INSTALL_TREE_SMOKE)
    add_test(NAME build/install-tree-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/run-install-tree-smoke.sh"
                --source "${CMAKE_SOURCE_DIR}"
                --build-dir "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_DIR}"
                --prefix "${CONFLUX_INSTALL_TREE_SMOKE_PREFIX}"
                --smoke-build-dir "${CONFLUX_INSTALL_TREE_SMOKE_CONSUMER_BUILD_DIR}"
                --feature-set "${CONFLUX_INSTALL_TREE_SMOKE_FEATURE_SET}"
                --build-type "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_TYPE}"
                --generator "${CONFLUX_INSTALL_TREE_SMOKE_GENERATOR}"
                --interface-mode "${CONFLUX_INSTALL_TREE_SMOKE_INTERFACE_MODE}"
                --components "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}"
                $<$<BOOL:${CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER}>:--mixed-module-header-smoke>
                -- ${CONFLUX_INSTALL_TREE_SMOKE_EXTRA_CMAKE_ARGS})
    set_tests_properties(build/install-tree-smoke PROPERTIES
        LABELS "build;package;install"
    )
endif()
