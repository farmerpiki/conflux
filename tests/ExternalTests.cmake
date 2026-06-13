if(CONFLUX_HAS_TLS STREQUAL "true")
    add_executable(conflux_tls_external)
    target_sources(conflux_tls_external
        PRIVATE
            tls_external.cxx
        PRIVATE FILE_SET CXX_MODULES FILES
            support.cxx
            external_support.cxx
    )
    target_link_libraries(conflux_tls_external
        PRIVATE
            conflux
            conflux_options
            Catch2::Catch2WithMain
    )

    if(CONFLUX_EFFECTIVE_SMTP)
        add_executable(conflux_smtp_tests smtp_test.cxx)
        target_link_libraries(conflux_smtp_tests
            PRIVATE conflux conflux_options Catch2::Catch2WithMain)
    endif()

    if(CONFLUX_BUILD_LIBCURL_EXTERNAL_TESTS AND CURL_FOUND)
        add_executable(conflux_libcurl_external)
        target_sources(conflux_libcurl_external
            PRIVATE
                libcurl_external.cxx
            PRIVATE FILE_SET CXX_MODULES FILES
                support.cxx
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
                support.cxx
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
            support.cxx
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
            support.cxx
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
