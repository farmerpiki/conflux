add_executable(conflux_json_tests json_test.cxx)
target_link_libraries(conflux_json_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)
add_executable(conflux_json_dom_path_tests json_dom_path_test.cxx)
target_link_libraries(conflux_json_dom_path_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)
add_executable(conflux_json_parse_container_tests json_parse_container_test.cxx)
target_link_libraries(conflux_json_parse_container_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)
add_executable(conflux_json_parse_scalar_tests json_parse_scalar_test.cxx)
target_link_libraries(conflux_json_parse_scalar_tests PRIVATE conflux conflux_options Catch2::Catch2WithMain)

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
    # Under P2996 -freflection, including Catch2 headers in any TU that
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
    target_compile_options(conflux_json_reflect_tests PRIVATE ${CONFLUX_REFLECTION_COMPILE_OPTIONS})
    add_test(NAME conflux_json_reflect_tests COMMAND conflux_json_reflect_tests)
    set_tests_properties(conflux_json_reflect_tests PROPERTIES RUN_SERIAL TRUE)
endif()

add_executable(conflux_json_conformance_external json_conformance_external.cxx)
target_link_libraries(conflux_json_conformance_external PRIVATE conflux conflux_options Catch2::Catch2WithMain)

if(CONFLUX_HAS_JSON_TESTSUITE)
    add_executable(conflux_json_testsuite_gate json_testsuite_gate.cxx)
    target_link_libraries(conflux_json_testsuite_gate PRIVATE conflux conflux_options Catch2::Catch2WithMain)
    target_compile_definitions(conflux_json_testsuite_gate PRIVATE "JSONTESTSUITE_DIR=\"${JSONTESTSUITE_DIR}\"")
endif()
