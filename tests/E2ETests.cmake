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

add_executable(conflux_http_proxy_e2e)
target_sources(conflux_http_proxy_e2e
    PRIVATE
        http_proxy_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_proxy_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_proxy_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_cookie_signing_e2e)
target_sources(conflux_http_cookie_signing_e2e
    PRIVATE
        http_cookie_signing_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_cookie_signing_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_cookie_signing_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_csrf_e2e)
target_sources(conflux_http_csrf_e2e
    PRIVATE
        http_csrf_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_csrf_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_csrf_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_etag_e2e)
target_sources(conflux_http_etag_e2e
    PRIVATE
        http_etag_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_etag_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_etag_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_response_cache_e2e)
target_sources(conflux_http_response_cache_e2e
    PRIVATE
        http_response_cache_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_response_cache_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_response_cache_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_tracing_e2e)
target_sources(conflux_http_tracing_e2e
    PRIVATE
        http_tracing_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_tracing_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_tracing_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_request_id_e2e)
target_sources(conflux_http_request_id_e2e
    PRIVATE
        http_request_id_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_request_id_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_request_id_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_request_semantics_e2e)
target_sources(conflux_http_request_semantics_e2e
    PRIVATE
        http_request_semantics_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_request_semantics_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_request_semantics_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_vhost_e2e)
target_sources(conflux_http_vhost_e2e
    PRIVATE
        http_vhost_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_vhost_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_vhost_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_openapi_e2e)
target_sources(conflux_http_openapi_e2e
    PRIVATE
        http_openapi_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_openapi_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_openapi_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_parser_rejection_e2e)
target_sources(conflux_http_parser_rejection_e2e
    PRIVATE
        http_parser_rejection_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_parser_rejection_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_parser_rejection_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_ws_validation_e2e)
target_sources(conflux_http_ws_validation_e2e
    PRIVATE
        http_ws_validation_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_ws_validation_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_ws_validation_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_tls_sniff_e2e)
target_sources(conflux_http_tls_sniff_e2e
    PRIVATE
        http_tls_sniff_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_tls_sniff_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_tls_sniff_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_deferred_sse_e2e)
target_sources(conflux_http_deferred_sse_e2e
    PRIVATE
        http_deferred_sse_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_deferred_sse_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_deferred_sse_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_shutdown_recv_e2e)
target_sources(conflux_http_shutdown_recv_e2e
    PRIVATE
        http_shutdown_recv_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_shutdown_recv_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_shutdown_recv_e2e
    PRIVATE
        conflux
        conflux_options
        Catch2::Catch2WithMain
)

add_executable(conflux_http_structured_log_e2e)
target_sources(conflux_http_structured_log_e2e
    PRIVATE
        http_structured_log_e2e.cxx
    PRIVATE FILE_SET CXX_MODULES FILES
        support.cxx
)
target_include_directories(conflux_http_structured_log_e2e PRIVATE "${CMAKE_SOURCE_DIR}/src/net")
target_link_libraries(conflux_http_structured_log_e2e
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
