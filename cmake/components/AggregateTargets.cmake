if(CONFLUX_WANT_HTTP_SERVER)

add_library(conflux STATIC)

target_sources(conflux
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/facade/conflux_curated.cxx
        ${CONFLUX_SRC_ROOT}/facade/conflux_extended.cxx
        ${CONFLUX_SRC_ROOT}/facade/conflux_complete.cxx
        ${CONFLUX_SRC_ROOT}/conflux.cppm
)

target_sources(conflux
    PUBLIC FILE_SET generated_mods TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
        FILES
        "${CMAKE_CURRENT_BINARY_DIR}/src/conflux_features.cxx"
)

target_link_libraries(conflux
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
    PUBLIC  conflux_net_config
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_json
    PUBLIC  conflux_http_response_json
    PUBLIC  conflux_http_app_json
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_static_core
    PUBLIC  conflux_http_static_async
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_policy
    PUBLIC  conflux_http_auth
    PUBLIC  conflux_http_observability
    PUBLIC  conflux_http_openapi
    PUBLIC  conflux_http_vhost
    PUBLIC  conflux_http_protocol
    PUBLIC  conflux_http_server
    PUBLIC  conflux_http_app
    PUBLIC  conflux_net_http
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PRIVATE conflux_direct_slot_pool
)
if(TARGET conflux_process)
    target_link_libraries(conflux PUBLIC conflux_process)
endif()
if(TARGET conflux_net_io_buffer)
    target_link_libraries(conflux PUBLIC conflux_net_io_buffer)
endif()
if(TARGET conflux_template)
    target_link_libraries(conflux PUBLIC conflux_template)
endif()
if(TARGET conflux_template_watch)
    target_link_libraries(conflux PUBLIC conflux_template_watch)
elseif(TARGET conflux_file_watch)
    target_link_libraries(conflux PUBLIC conflux_file_watch)
endif()
if(TARGET conflux_json_boundary)
    target_link_libraries(conflux PUBLIC conflux_json_boundary)
endif()
if(TARGET conflux_http_parse_helpers)
    target_link_libraries(conflux PUBLIC conflux_http_parse_helpers)
endif()
if(TARGET conflux_json)
    target_link_libraries(conflux PUBLIC conflux_json)
endif()
if(TARGET conflux_json_native_provider)
    target_link_libraries(conflux PUBLIC conflux_json_native_provider)
endif()
if(TARGET conflux_json_reflect)
    target_link_libraries(conflux PUBLIC conflux_json_reflect)
endif()
if(TARGET conflux_json_reflect_provider)
    target_link_libraries(conflux PUBLIC conflux_json_reflect_provider)
endif()
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux PUBLIC conflux_http_native_json)
endif()
if(TARGET conflux_json_file)
    target_link_libraries(conflux PUBLIC conflux_json_file)
endif()
if(TARGET conflux_crypto)
    target_link_libraries(conflux PUBLIC conflux_crypto)
endif()
if(TARGET conflux_file_map)
    target_link_libraries(conflux PUBLIC conflux_file_map)
endif()
if(TARGET conflux_dns)
    target_link_libraries(conflux PUBLIC conflux_dns)
endif()
if(TARGET conflux_net_cancel)
    target_link_libraries(conflux PUBLIC conflux_net_cancel)
endif()
if(TARGET conflux_net_tls)
    target_link_libraries(conflux PUBLIC conflux_net_tls)
endif()
if(TARGET conflux_dns_bridge)
    target_link_libraries(conflux PUBLIC conflux_dns_bridge)
endif()
if(TARGET conflux_net_client)
    target_link_libraries(conflux PUBLIC conflux_net_client)
endif()
if(TARGET conflux_net_async_client)
    target_link_libraries(conflux PUBLIC conflux_net_async_client)
endif()
if(CONFLUX_WANT_HTTP_COMPRESSION AND TARGET conflux_http_compression)
    target_link_libraries(conflux PUBLIC conflux_http_compression)
endif()
if(TARGET conflux_http_proxy)
    target_link_libraries(conflux PUBLIC conflux_http_proxy)
endif()
if(TARGET conflux_net_smtp)
    target_link_libraries(conflux PUBLIC conflux_net_smtp)
endif()
if(TARGET conflux_pg)
    target_link_libraries(conflux PUBLIC conflux_pg)
endif()

if(CONFLUX_JSON_AVX2)
    target_compile_options(conflux PRIVATE -mavx2 -mbmi2)
endif()

conflux_apply_api_surface_definitions(conflux PUBLIC)
target_compile_definitions(conflux
    PUBLIC
        CONFLUX_HAS_JSON=$<BOOL:${CONFLUX_WANT_JSON}>
        CONFLUX_HAS_JSON_FILE=$<BOOL:${CONFLUX_WANT_JSON_FILE}>
        CONFLUX_HAS_TEMPLATES=$<BOOL:${CONFLUX_WANT_TEMPLATES}>
        CONFLUX_HAS_TEMPLATES_WATCH=$<BOOL:${CONFLUX_WANT_TEMPLATES_WATCH}>
        CONFLUX_HAS_FILE_WATCH=$<BOOL:${CONFLUX_WANT_FILE_WATCH}>
)

if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()

endif()
