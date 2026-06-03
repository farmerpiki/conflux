if(CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_app STATIC)
target_sources(conflux_http_app
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/app_defer.cxx
            ${CONFLUX_SRC_ROOT}/net/app_extractor_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_json_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_metadata_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_policies.cxx
            ${CONFLUX_SRC_ROOT}/net/app_response.cxx
            ${CONFLUX_SRC_ROOT}/net/app_route_helpers.cxx
            ${CONFLUX_SRC_ROOT}/net/app_traits.cxx
            ${CONFLUX_SRC_ROOT}/net/app_types.cxx
            ${CONFLUX_SRC_ROOT}/net/app.cxx
)
target_link_libraries(conflux_http_app
    PRIVATE conflux_options
    PRIVATE conflux_file_io_sync
    PUBLIC  conflux_types
    PUBLIC  conflux_net_config
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_parse_helpers
    PUBLIC  conflux_http_response
    PUBLIC  conflux_json
    PUBLIC  conflux_json_boundary
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_router_match
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_server
    PUBLIC  conflux_crypto
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
    PUBLIC  conflux_utils
)
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux_http_app PUBLIC conflux_http_native_json)
    target_compile_definitions(conflux_http_app PUBLIC CONFLUX_HAS_JSON=1)
else()
    target_compile_definitions(conflux_http_app PUBLIC CONFLUX_HAS_JSON=0)
endif()
target_link_libraries(conflux_http_app
    PUBLIC  conflux_http_json
    PUBLIC  conflux_http_response_json
    PUBLIC  conflux_net_client
    PUBLIC  conflux_http_protocol
    PUBLIC  conflux_http_auth
    PUBLIC  conflux_http_compression
    PUBLIC  conflux_http_policy
    PUBLIC  conflux_http_observability
    PUBLIC  conflux_http_openapi
    PUBLIC  conflux_http_vhost
)
if(TARGET conflux_http2)
    target_link_libraries(conflux_http_app PUBLIC conflux_http2)
endif()
if(TARGET conflux_http3)
    target_link_libraries(conflux_http_app PUBLIC conflux_http3)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (app facade)

if(TARGET conflux_http_response_json AND TARGET conflux_http_app AND TARGET conflux_http_router)
target_sources(conflux_http_app
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_app_json.cxx
)
add_library(conflux_http_app_json INTERFACE)
target_link_libraries(conflux_http_app_json
    INTERFACE conflux_http_app
    INTERFACE conflux_http_router
    INTERFACE conflux_http_response_json
)
endif() # conflux_http_response_json && conflux_http_app && conflux_http_router
