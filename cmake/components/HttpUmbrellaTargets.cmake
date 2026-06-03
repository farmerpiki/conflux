if(CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_net_http STATIC)
target_sources(conflux_net_http
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/http_facade_problem.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade.cxx
            ${CONFLUX_SRC_ROOT}/net/http_facade_extended.cxx
            ${CONFLUX_SRC_ROOT}/net/http_server_umbrella.cxx
            ${CONFLUX_SRC_ROOT}/net/http.cppm
)
target_link_libraries(conflux_net_http
    PRIVATE conflux_options
    PUBLIC conflux_types
    PUBLIC conflux_net_config
    PUBLIC conflux_file_io_sync
    PUBLIC conflux_http_core
    PUBLIC conflux_http_json
    PUBLIC conflux_http_realtime
    PUBLIC conflux_http_response
    PUBLIC conflux_http_response_json
    PUBLIC conflux_http_app_json
    PUBLIC conflux_http_router
    PUBLIC conflux_net_client
    PUBLIC conflux_http_protocol
    PUBLIC conflux_http_server
    PUBLIC conflux_http_app
    PUBLIC conflux_http_auth
    PUBLIC conflux_http_compression
    PUBLIC conflux_http_policy
    PUBLIC conflux_http_observability
    PUBLIC conflux_http_openapi
    PUBLIC conflux_http_vhost
    PUBLIC conflux_utils
    PUBLIC conflux_work
)
if(TARGET conflux_http_native_json)
    target_link_libraries(conflux_net_http PUBLIC conflux_http_native_json)
endif()
if(TARGET conflux_json)
    target_link_libraries(conflux_net_http PUBLIC conflux_json)
endif()
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_METRICS STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_METRICS=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_METRICS=0)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP2=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP2=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_net_http PUBLIC CONFLUX_HAS_HTTP3=0)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (HTTP umbrella)
