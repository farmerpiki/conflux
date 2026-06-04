add_library(conflux_direct_slot_pool STATIC)
target_sources(conflux_direct_slot_pool
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
        ${CONFLUX_SRC_ROOT}/net/direct_slot_pool.cxx
)
target_link_libraries(conflux_direct_slot_pool
    PRIVATE conflux_options
    PRIVATE conflux_types
    PRIVATE conflux_utils)

if(CONFLUX_WANT_HTTP_SERVER)
add_library(conflux_http_server_helpers STATIC)
target_sources(conflux_http_server_helpers
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server_helpers.cxx
)
target_link_libraries(conflux_http_server_helpers
    PRIVATE conflux_options
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_response
    PUBLIC  conflux_utils
    PUBLIC  conflux_http_parse_helpers
)

add_library(conflux_http_server_config STATIC)
target_sources(conflux_http_server_config
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server_config.cxx
)
target_link_libraries(conflux_http_server_config
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_uring
    PUBLIC  conflux_net_config
)

add_library(conflux_http_server STATIC)
set(_conflux_http_server_private_partitions
    ${CONFLUX_SRC_ROOT}/net/http_server_init.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_state.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_tls.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_send.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_ws.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_recv.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_cqe.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_diag.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_loop.cxx
    ${CONFLUX_SRC_ROOT}/net/http_server_dispatch.cxx
)
conflux_apply_http_server_compiler_workarounds("${CONFLUX_SRC_ROOT}/net/http_server_send.cxx")
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    list(APPEND _conflux_http_server_private_partitions
        ${CONFLUX_SRC_ROOT}/net/http_server_h2.cxx
    )
endif()
target_sources(conflux_http_server
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/http_server.cxx
)
target_sources(conflux_http_server
    PRIVATE FILE_SET private_http_server_partitions TYPE CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${_conflux_http_server_private_partitions}
)
target_sources(conflux_http_server
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/http_server_impl.cxx
)
target_link_libraries(conflux_http_server
    PRIVATE conflux_options
    PRIVATE conflux_http_server_helpers
    PRIVATE conflux_http_server_config
    PUBLIC  conflux_types
    PUBLIC  conflux_http_core
    PUBLIC  conflux_http_router
    PUBLIC  conflux_http_static
    PUBLIC  conflux_http_realtime
    PUBLIC  conflux_http_vhost
    PUBLIC  conflux_http_protocol
    PUBLIC  conflux_net_config
    PUBLIC  conflux_file_map
    PRIVATE conflux_direct_slot_pool
    PUBLIC  conflux_uring
    PUBLIC  conflux_work
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_utils
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_http_server PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_HTTP2 STREQUAL "true")
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP2=1)
else()
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP2=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_HAS_HTTP3=0)
endif()
if(CONFLUX_BUILD_FUZZ)
    target_compile_definitions(conflux_http_server PUBLIC CONFLUX_BUILD_FUZZ=1)
endif()
endif() # CONFLUX_WANT_HTTP_SERVER (server loop)
