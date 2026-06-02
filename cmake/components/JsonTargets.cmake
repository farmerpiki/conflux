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
        ${CONFLUX_SRC_ROOT}/json_dom.cxx
        ${CONFLUX_SRC_ROOT}/json_lookup.cxx
        ${CONFLUX_SRC_ROOT}/json_dump.cxx
        ${CONFLUX_SRC_ROOT}/json_parse.cxx
        ${CONFLUX_SRC_ROOT}/json_builder.cxx
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
