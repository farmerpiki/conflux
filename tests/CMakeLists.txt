find_package(Catch2 3 REQUIRED)
include(Catch)

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

add_executable(conflux_utils_tests utils_test.cxx)
target_link_libraries(conflux_utils_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_template_tests template_test.cxx)
target_link_libraries(conflux_template_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_json_tests json_test.cxx)
target_link_libraries(conflux_json_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_process_tests process_test.cxx)
target_link_libraries(conflux_process_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

add_executable(conflux_json_conformance_external json_conformance_external.cxx)
target_link_libraries(conflux_json_conformance_external PRIVATE conflux conflux_options Catch2::Catch2WithMain)
catch_discover_tests(conflux_json_conformance_external)

add_executable(conflux_work_tests work_test.cxx)
target_link_libraries(conflux_work_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_root_tests work_root_test.cxx)
target_link_libraries(conflux_work_root_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_tests work_carrier_test.cxx)
target_link_libraries(conflux_work_carrier_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase2_tests work_carrier_phase2_test.cxx)
target_link_libraries(conflux_work_carrier_phase2_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_work_carrier_phase3_tests work_carrier_phase3_test.cxx)
target_link_libraries(conflux_work_carrier_phase3_tests PRIVATE conflux_work conflux_options Catch2::Catch2WithMain)

add_executable(conflux_file_io_tests file_io_test.cxx)
target_link_libraries(conflux_file_io_tests
    PRIVATE
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
catch_discover_tests(conflux_tests)
if(CONFLUX_HAS_TLS STREQUAL "true")
    catch_discover_tests(conflux_tls_external)
endif()
catch_discover_tests(conflux_crypto_tests)
catch_discover_tests(conflux_utils_tests)
catch_discover_tests(conflux_template_tests)
catch_discover_tests(conflux_json_tests)
catch_discover_tests(conflux_process_tests)
catch_discover_tests(conflux_work_tests)
catch_discover_tests(conflux_work_root_tests)
catch_discover_tests(conflux_work_carrier_tests)
catch_discover_tests(conflux_work_carrier_phase2_tests)
catch_discover_tests(conflux_work_carrier_phase3_tests)
catch_discover_tests(conflux_file_io_tests)
catch_discover_tests(conflux_file_io_http_e2e)
if(CONFLUX_HAS_TLS STREQUAL "true")
    catch_discover_tests(conflux_smtp_tests)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    catch_discover_tests(conflux_h2_external)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    catch_discover_tests(conflux_h3_external)
endif()
if(CONFLUX_HAS_DB STREQUAL "true")
    catch_discover_tests(conflux_db_tests)
    catch_discover_tests(conflux_db_integration)
endif()
