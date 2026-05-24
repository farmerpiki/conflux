set(_conflux_examples_root "${CONFLUX_EXAMPLES_SOURCE_ROOT}")

set(CONFLUX_EXAMPLES_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/conflux_examples_manifest.txt")
file(WRITE "${CONFLUX_EXAMPLES_MANIFEST}" "Conflux examples manifest\n\n")
function(conflux_note_example TARGET_NAME STATUS REASON)
    file(APPEND "${CONFLUX_EXAMPLES_MANIFEST}" "${TARGET_NAME}: ${STATUS} - ${REASON}\n")
endfunction()

if(TARGET conflux)
    add_executable(conflux_quickstart_hello ${_conflux_examples_root}/quickstart/hello.cxx)
    target_link_libraries(conflux_quickstart_hello PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_middleware ${_conflux_examples_root}/quickstart/middleware.cxx)
    target_link_libraries(conflux_quickstart_middleware PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_sse ${_conflux_examples_root}/quickstart/sse.cxx)
    target_link_libraries(conflux_quickstart_sse PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_static_files ${_conflux_examples_root}/quickstart/static_files.cxx)
    target_link_libraries(conflux_quickstart_static_files PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_websocket ${_conflux_examples_root}/quickstart/websocket.cxx)
    target_link_libraries(conflux_quickstart_websocket PRIVATE conflux conflux_options)

    add_executable(conflux_quickstart_openapi ${_conflux_examples_root}/quickstart/openapi.cxx)
    target_link_libraries(conflux_quickstart_openapi PRIVATE conflux conflux_options)

    if(TARGET conflux_http_native_json AND TARGET conflux_json)
        add_executable(conflux_quickstart_json_crud ${_conflux_examples_root}/quickstart/json_crud.cxx)
        target_link_libraries(conflux_quickstart_json_crud PRIVATE conflux conflux_options)
    endif()

    if(CONFLUX_JSON_REFLECT AND TARGET conflux_json_reflect)
        add_executable(conflux_quickstart_json_reflect_crud ${_conflux_examples_root}/quickstart/json_reflect_crud.cxx)
        target_link_libraries(conflux_quickstart_json_reflect_crud PRIVATE conflux conflux_json_reflect conflux_options)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true" AND TARGET conflux_db)
        add_executable(conflux_quickstart_postgres ${_conflux_examples_root}/quickstart/postgres_json.cxx)
        target_link_libraries(conflux_quickstart_postgres
            PRIVATE conflux conflux_pg conflux_file_io conflux_work conflux_options)
    endif()

    # Easy HTTP examples (first-contact / v1-facing)
    add_executable(conflux_hello ${_conflux_examples_root}/hello.cxx)
    target_link_libraries(conflux_hello PRIVATE conflux conflux_options)

    add_executable(conflux_middleware ${_conflux_examples_root}/middleware.cxx)
    target_link_libraries(conflux_middleware PRIVATE conflux conflux_options)

    add_executable(conflux_sse ${_conflux_examples_root}/sse.cxx)
    target_link_libraries(conflux_sse PRIVATE conflux conflux_options)

    add_executable(conflux_static ${_conflux_examples_root}/static.cxx)
    target_link_libraries(conflux_static PRIVATE conflux conflux_options)

    # Additional examples
    add_executable(conflux_forms ${_conflux_examples_root}/forms.cxx)
    target_link_libraries(conflux_forms PRIVATE conflux conflux_options)

    add_executable(conflux_gzip ${_conflux_examples_root}/gzip.cxx)
    target_link_libraries(conflux_gzip PRIVATE conflux conflux_options)

    add_executable(conflux_http_client ${_conflux_examples_root}/http_client.cxx)
    target_link_libraries(conflux_http_client PRIVATE conflux conflux_options)

    add_executable(conflux_dual ${_conflux_examples_root}/advanced/dual.cxx)
    target_link_libraries(conflux_dual PRIVATE conflux conflux_options)

    add_executable(conflux_process_run_example ${_conflux_examples_root}/advanced/process_run.cxx)
    target_link_libraries(conflux_process_run_example PRIVATE conflux conflux_options)

    add_executable(conflux_crypto_sealing_example ${_conflux_examples_root}/advanced/crypto_sealing.cxx)
    target_link_libraries(conflux_crypto_sealing_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_observability_example ${_conflux_examples_root}/advanced/http_observability.cxx)
    target_link_libraries(conflux_http_observability_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_lifecycle_example ${_conflux_examples_root}/advanced/http_lifecycle.cxx)
    target_link_libraries(conflux_http_lifecycle_example PRIVATE conflux conflux_options)

    add_executable(conflux_production_showcase_example ${_conflux_examples_root}/advanced/production_showcase.cxx)
    target_link_libraries(conflux_production_showcase_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_policy_stack_example ${_conflux_examples_root}/advanced/http_policy_stack.cxx)
    target_link_libraries(conflux_http_policy_stack_example PRIVATE conflux conflux_options)

    add_executable(conflux_vhost_openapi_example ${_conflux_examples_root}/advanced/vhost_openapi.cxx)
    target_link_libraries(conflux_vhost_openapi_example PRIVATE conflux conflux_options)

    add_executable(conflux_http_client_builder_example ${_conflux_examples_root}/advanced/http_client_builder.cxx)
    target_link_libraries(conflux_http_client_builder_example PRIVATE conflux conflux_options)

    if(TARGET conflux_http_app_json AND TARGET conflux_http_native_json AND TARGET conflux_json)
        add_executable(conflux_api_typed_json_example ${_conflux_examples_root}/advanced/manual_json_members.cxx)
        target_link_libraries(conflux_api_typed_json_example
            PRIVATE conflux conflux_options)

        add_executable(conflux_http_explicit_offload_example ${_conflux_examples_root}/advanced/explicit_offload.cxx)
        target_link_libraries(conflux_http_explicit_offload_example
            PRIVATE conflux conflux_work conflux_options)
    endif()

    if(TARGET conflux_http_native_json AND TARGET conflux_json)
        add_executable(conflux_http_client_json_example ${_conflux_examples_root}/advanced/http_client_json.cxx)
        target_link_libraries(conflux_http_client_json_example
            PRIVATE conflux_http_native_json conflux_json conflux_options)

        add_executable(conflux_custom_json_provider_example ${_conflux_examples_root}/advanced/custom_json_provider.cxx)
        target_link_libraries(conflux_custom_json_provider_example
            PRIVATE conflux_http_native_json conflux_json conflux_options)
    endif()

    add_executable(conflux_work_join_all_example ${_conflux_examples_root}/advanced/work_join_all.cxx)
    target_link_libraries(conflux_work_join_all_example PRIVATE conflux conflux_options)

    if(CONFLUX_WANT_TEMPLATES AND TARGET conflux_template)
        add_executable(conflux_template_pages_example ${_conflux_examples_root}/advanced/template_pages.cxx)
        target_link_libraries(conflux_template_pages_example PRIVATE conflux conflux_template conflux_options)
    endif()

    if(CONFLUX_HAS_HTTP3 STREQUAL "true")
        add_executable(conflux_h3_probe ${_conflux_examples_root}/advanced/h3_probe.cxx)
        target_link_libraries(conflux_h3_probe PRIVATE conflux conflux_options)
        add_executable(conflux_h3_server ${_conflux_examples_root}/advanced/h3_server.cxx)
        target_link_libraries(conflux_h3_server PRIVATE conflux conflux_options)
    endif()

    if(CONFLUX_HAS_DB STREQUAL "true" AND TARGET conflux_db)
        add_executable(conflux_db_basic ${_conflux_examples_root}/advanced/db_basic.cxx)
        target_link_libraries(conflux_db_basic
            PRIVATE conflux_pg conflux_file_io conflux_work conflux_options)

        add_executable(conflux_db_pool ${_conflux_examples_root}/advanced/db_pool.cxx)
        target_link_libraries(conflux_db_pool
            PRIVATE conflux_pg conflux_file_io conflux_work conflux_options)

        add_executable(conflux_advanced_postgres ${_conflux_examples_root}/advanced/postgres.cxx)
        target_link_libraries(conflux_advanced_postgres
            PRIVATE conflux_pg conflux_file_io conflux_work conflux_options)
    endif()

    # JSON examples (use monolith for convenience)
    add_executable(conflux_json_example ${_conflux_examples_root}/advanced/json.cxx)
    target_link_libraries(conflux_json_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_config_example ${_conflux_examples_root}/advanced/json_config.cxx)
    target_link_libraries(conflux_json_config_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_stream_ingest_example ${_conflux_examples_root}/advanced/json_stream_ingest.cxx)
    target_link_libraries(conflux_json_stream_ingest_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_diagnostics_example ${_conflux_examples_root}/advanced/json_diagnostics.cxx)
    target_link_libraries(conflux_json_diagnostics_example PRIVATE conflux conflux_options)

    add_executable(conflux_json_transform_example ${_conflux_examples_root}/advanced/json_transform.cxx)
    target_link_libraries(conflux_json_transform_example PRIVATE conflux conflux_options)
endif()

# ---------------------------------------------------------------------------
# Standalone examples (don't require monolith)
# ---------------------------------------------------------------------------

if(NOT COMMAND conflux_note_example)
    set(CONFLUX_EXAMPLES_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/conflux_examples_manifest.txt")
    file(WRITE "${CONFLUX_EXAMPLES_MANIFEST}" "Conflux examples manifest\n\n")
    function(conflux_note_example TARGET_NAME STATUS REASON)
        file(APPEND "${CONFLUX_EXAMPLES_MANIFEST}" "${TARGET_NAME}: ${STATUS} - ${REASON}\n")
    endfunction()
endif()

if(CONFLUX_WANT_FILE_IO)
    add_executable(conflux_file_io_example ${_conflux_examples_root}/advanced/file_io.cxx)
    target_link_libraries(conflux_file_io_example PRIVATE conflux_file_io conflux_work conflux_options)

    add_executable(conflux_coroutines ${_conflux_examples_root}/advanced/coroutines.cxx)
    target_link_libraries(conflux_coroutines PRIVATE conflux_file_io conflux_work conflux_options)
endif()

if(CONFLUX_JSON_REFLECT AND TARGET conflux_json_reflect)
    add_executable(conflux_json_reflect_example ${_conflux_examples_root}/advanced/json_reflect.cxx)
    target_link_libraries(conflux_json_reflect_example
        PRIVATE conflux_json_reflect_provider conflux_json_reflect conflux_json conflux_types conflux_options)
endif()

set(CONFLUX_AVAILABLE_EXAMPLE_TARGETS)
get_property(_conflux_examples_dir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_conflux_example_target IN LISTS _conflux_examples_dir_targets)
    get_target_property(_conflux_example_type ${_conflux_example_target} TYPE)
    if(_conflux_example_type STREQUAL "EXECUTABLE")
        list(APPEND CONFLUX_AVAILABLE_EXAMPLE_TARGETS ${_conflux_example_target})
        conflux_note_example(${_conflux_example_target} built "example target")
    endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CONFLUX_GCC_EAGER_MODULE_IMPORTS)
    foreach(_conflux_example_target IN LISTS CONFLUX_AVAILABLE_EXAMPLE_TARGETS)
        target_compile_options(${_conflux_example_target} PRIVATE -fno-module-lazy)
    endforeach()
endif()

add_custom_target(conflux_examples)
if(CONFLUX_AVAILABLE_EXAMPLE_TARGETS)
    add_dependencies(conflux_examples ${CONFLUX_AVAILABLE_EXAMPLE_TARGETS})
endif()

if(CONFLUX_BUILD_TESTS)
    add_test(NAME examples/quickstart-policy
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-quickstart-examples.sh"
                "${CMAKE_SOURCE_DIR}/examples/quickstart"
    )
    set_tests_properties(examples/quickstart-policy PROPERTIES
        LABELS "examples;lint"
    )

    add_test(NAME examples/public-policy
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-public-examples.sh"
                "${CMAKE_SOURCE_DIR}/examples"
    )
    set_tests_properties(examples/public-policy PROPERTIES
        LABELS "examples;lint"
    )

    add_test(NAME examples/compile
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                --target conflux_examples --config "$<CONFIG>"
    )
    set_tests_properties(examples/compile PROPERTIES
        LABELS "examples;build"
        RUN_SERIAL TRUE
    )

    function(conflux_add_example_e2e CASE_NAME TARGET_NAME)
        if(TARGET ${TARGET_NAME})
            add_test(NAME examples/e2e-${CASE_NAME}
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/scripts/run-example-e2e.py"
                        --case "${CASE_NAME}"
                        --exe "$<TARGET_FILE:${TARGET_NAME}>"
            )
            set_tests_properties(examples/e2e-${CASE_NAME} PROPERTIES
                LABELS "examples;e2e"
                RUN_SERIAL TRUE
                TIMEOUT 15
            )
        endif()
    endfunction()

    conflux_add_example_e2e(quickstart_hello conflux_quickstart_hello)
    conflux_add_example_e2e(quickstart_json_crud conflux_quickstart_json_crud)
    conflux_add_example_e2e(quickstart_static_files conflux_quickstart_static_files)
    conflux_add_example_e2e(quickstart_sse conflux_quickstart_sse)
    conflux_add_example_e2e(quickstart_websocket conflux_quickstart_websocket)
    conflux_add_example_e2e(production_showcase conflux_production_showcase_example)
endif()
