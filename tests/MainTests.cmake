add_executable(conflux_tests)
set_source_files_properties(
    http_e2e_middleware.cxx
    http_e2e_observability.cxx
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(conflux_tests
    PRIVATE
        http_e2e.cxx
        http_app_e2e.cxx
        http_router_e2e.cxx
        http_lifecycle_e2e.cxx
        http_client_e2e.cxx
        http_realtime_e2e.cxx
        http_form_e2e.cxx
        http_static_e2e.cxx
        http_fields_test.cxx
        http_router_unit_test.cxx
        http1_parser_test.cxx
        http_compress_e2e.cxx
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
