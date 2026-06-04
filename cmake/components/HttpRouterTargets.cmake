if(CONFLUX_HTTP_REALTIME_TARGET_REQUESTED)
add_library(conflux_http_realtime STATIC)
target_sources(conflux_http_realtime
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/realtime.cxx
)
target_link_libraries(conflux_http_realtime
    PRIVATE conflux_options
    PRIVATE conflux_cpu_features
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_crypto
    PUBLIC  conflux_utils
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_http_realtime PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_http_realtime PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_http_realtime PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(NOT _conflux_simd_backend STREQUAL "OFF")
    target_compile_definitions(conflux_http_realtime PRIVATE CONFLUX_STDSIMD=1)
    target_link_libraries(conflux_http_realtime PRIVATE conflux_simd_runtime)
endif()
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux_http_realtime PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()
endif() # CONFLUX_HTTP_REALTIME_TARGET_REQUESTED

if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
add_library(conflux_http_response STATIC)
target_sources(conflux_http_response
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/response.cxx
)
target_link_libraries(conflux_http_response
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_file_map
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_utils
)
endif() # CONFLUX_HTTP_ROUTER_STACK_REQUESTED (HTTP response vocabulary)

if(TARGET conflux_http_response AND TARGET conflux_http_json)
add_library(conflux_http_response_json STATIC)
target_sources(conflux_http_response_json
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_response_json.cxx
)
target_link_libraries(conflux_http_response_json
    PRIVATE conflux_options
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_json
    PUBLIC  conflux_http_response
    PUBLIC  conflux_utils
)
endif() # conflux_http_response && conflux_http_json

if(TARGET conflux_http_response_json AND TARGET conflux_json_native_provider)
add_library(conflux_http_native_json STATIC)
target_sources(conflux_http_native_json
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_native_json.cxx
)
target_link_libraries(conflux_http_native_json
    PRIVATE conflux_options
    PUBLIC  conflux_http_response_json
    PUBLIC  conflux_json_native_provider
)
endif() # conflux_http_response_json && conflux_json_native_provider

if(CONFLUX_HTTP_STATIC_TARGET_REQUESTED)
add_library(conflux_http_static_core STATIC)
target_sources(conflux_http_static_core
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/static_core.cxx
)
target_link_libraries(conflux_http_static_core
    PRIVATE conflux_options
    PUBLIC  conflux_types
)

add_library(conflux_http_static STATIC)
target_sources(conflux_http_static
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/static.cxx
)
target_link_libraries(conflux_http_static
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_net_config
)
endif() # CONFLUX_HTTP_STATIC_TARGET_REQUESTED

if(CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
add_library(conflux_http_static_async STATIC)
target_sources(conflux_http_static_async
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/static_async_iface.cxx
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/static_async.cxx
)
target_link_libraries(conflux_http_static_async
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_file_map
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_static_core
    PUBLIC  conflux_utils
)

add_library(conflux_http_router STATIC)
target_sources(conflux_http_router
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/path.cxx
            ${CONFLUX_SRC_ROOT}/net/router.cxx
)
target_sources(conflux_http_router
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/router_impl.cxx
)
add_library(conflux_http_router_dispatch STATIC)
target_sources(conflux_http_router_dispatch
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router_dispatch.cxx
)
target_compile_definitions(conflux_http_router_dispatch PRIVATE
    CONFLUX_ROUTER_LAZY_ROUTE_METADATA=$<BOOL:${CONFLUX_ROUTER_LAZY_ROUTE_METADATA}>)
target_link_libraries(conflux_http_router_dispatch
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_router_match
    PUBLIC  conflux_work
)
add_library(conflux_http_router_match STATIC)
target_sources(conflux_http_router_match
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router_match.cxx
)
target_link_libraries(conflux_http_router_match
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_utils
)
add_library(conflux_http_router_static STATIC)
target_sources(conflux_http_router_static
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/router_static.cxx
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/router_static_impl.cxx
)
target_link_libraries(conflux_http_router_static
    PRIVATE conflux_options
    PRIVATE conflux_http_static_async
    PRIVATE conflux_file_io_sync
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_static_core
    PUBLIC  conflux_net_config
)
target_link_libraries(conflux_http_router
    PRIVATE conflux_options
    PRIVATE conflux_http_router_dispatch
    PRIVATE conflux_http_router_match
    PRIVATE conflux_http_router_static
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_response
    PRIVATE conflux_http_static_core
    PRIVATE conflux_http_static_async
    PUBLIC  conflux_work
    PUBLIC  conflux_utils
    PUBLIC  conflux_net_config
    PRIVATE conflux_socket_io
)
endif() # HTTP router or router-dependent component
