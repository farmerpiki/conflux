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
