if(CONFLUX_WANT_JSON OR CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
conflux_add_module_library(conflux_json_boundary
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/json_boundary.cxx
)
target_link_libraries(conflux_json_boundary
    PUBLIC  conflux_types
    PRIVATE conflux_options
)
endif()

if(CONFLUX_WANT_JSON)
conflux_add_module_library(conflux_json
    PUBLIC_MODULES
        ${CONFLUX_SRC_ROOT}/json.cppm
        ${CONFLUX_SRC_ROOT}/json_api.cxx
        ${CONFLUX_SRC_ROOT}/json_codec.cxx
)
target_compile_features(conflux_json PUBLIC cxx_std_23)
target_sources(conflux_json
    PRIVATE
        ${CONFLUX_SRC_ROOT}/json_libc.cxx
        ${CONFLUX_SRC_ROOT}/json_storage.cxx
        ${CONFLUX_SRC_ROOT}/json_number.cxx
        ${CONFLUX_SRC_ROOT}/json_reader.cxx
        ${CONFLUX_SRC_ROOT}/json_reader_strings.cxx
        ${CONFLUX_SRC_ROOT}/json_reader_object_values.cxx
        ${CONFLUX_SRC_ROOT}/json_dom.cxx
        ${CONFLUX_SRC_ROOT}/json_lookup.cxx
        ${CONFLUX_SRC_ROOT}/json_dump.cxx
        ${CONFLUX_SRC_ROOT}/json_parse.cxx
        ${CONFLUX_SRC_ROOT}/json_builder.cxx
        ${CONFLUX_SRC_ROOT}/json_patch.cxx
        ${CONFLUX_SRC_ROOT}/json_stream.cxx
)
target_link_libraries(conflux_json
    PUBLIC  conflux_types
    PRIVATE conflux_options
    PRIVATE conflux_cpu_features
)
target_compile_definitions(conflux_json PUBLIC CONFLUX_HAS_JSON=1)
if(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "XXHASH")
    target_link_libraries(conflux_json PRIVATE PkgConfig::XXHASH)
    target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_HASH_PROVIDER_XXHASH=1)
elseif(CONFLUX_JSON_HASH_PROVIDER_UPPER STREQUAL "INTERNAL")
    target_compile_definitions(conflux_json PRIVATE CONFLUX_JSON_HASH_PROVIDER_INTERNAL=1)
endif()

conflux_add_module_library(conflux_json_native_provider
    PUBLIC_MODULES ${CONFLUX_SRC_ROOT}/json_native_provider.cxx
)
target_link_libraries(conflux_json_native_provider
    PRIVATE conflux_options
    PUBLIC  conflux_json_boundary conflux_json
)
endif()

if(CONFLUX_JSON_REFLECT AND TARGET conflux_json)
    foreach(_conflux_json_reflect_dep IN ITEMS
            conflux_types
            conflux_json
            conflux_json_boundary
            conflux_json_native_provider)
        if(TARGET ${_conflux_json_reflect_dep})
            target_compile_options(${_conflux_json_reflect_dep} PUBLIC ${CONFLUX_REFLECTION_COMPILE_OPTIONS})
        endif()
    endforeach()

    add_library(conflux_json_reflect STATIC)
    target_compile_features(conflux_json_reflect PUBLIC cxx_std_26)
    target_sources(conflux_json_reflect
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES ${CONFLUX_SRC_ROOT}/json_reflect.cxx
    )
    target_link_libraries(conflux_json_reflect
        PUBLIC  conflux_json conflux_types
        PRIVATE conflux_options
    )
    target_compile_options(conflux_json_reflect PUBLIC ${CONFLUX_REFLECTION_COMPILE_OPTIONS})

    add_library(conflux_json_reflect_provider STATIC)
    target_sources(conflux_json_reflect_provider
        PUBLIC FILE_SET CXX_MODULES
            BASE_DIRS "${CONFLUX_SRC_ROOT}"
            FILES ${CONFLUX_SRC_ROOT}/json_reflect_provider.cxx
    )
    target_link_libraries(conflux_json_reflect_provider
        PUBLIC  conflux_json_reflect conflux_json_native_provider conflux_json_boundary
        PRIVATE conflux_options
    )
    target_compile_options(conflux_json_reflect_provider PUBLIC ${CONFLUX_REFLECTION_COMPILE_OPTIONS})
    message(STATUS "conflux: JSON P2996 reflection codec enabled (${CONFLUX_REFLECTION_COMPILE_OPTIONS})")
endif()
