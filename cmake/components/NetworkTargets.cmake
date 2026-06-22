add_library(conflux_net_config STATIC)
target_sources(conflux_net_config
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/config.cxx
)
target_link_libraries(conflux_net_config
    PRIVATE conflux_options
    PUBLIC  conflux_file_io_sync
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
)
if(CONFLUX_HAS_TLS STREQUAL "true")
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_TLS=1)
else()
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_TLS=0)
endif()
if(CONFLUX_HAS_HTTP3 STREQUAL "true")
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_HTTP3=1)
else()
    target_compile_definitions(conflux_net_config PUBLIC CONFLUX_HAS_HTTP3=0)
endif()

if(CONFLUX_HAS_TLS STREQUAL "true"
        AND (CONFLUX_HTTP_ROUTER_STACK_REQUESTED OR CONFLUX_HTTP_CLIENT_STACK_REQUESTED
             OR CONFLUX_EFFECTIVE_SMTP))
add_library(conflux_net_tls STATIC)
target_sources(conflux_net_tls
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/tls.cxx
)
target_link_libraries(conflux_net_tls
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
    PUBLIC  conflux_uring
    PUBLIC  conflux_file_io
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_net_cancel
    PUBLIC  OpenSSL::SSL
    PUBLIC  OpenSSL::Crypto
)
target_compile_definitions(conflux_net_tls PUBLIC CONFLUX_HAS_TLS=1)
endif() # CONFLUX_HAS_TLS && HTTP/TLS consumer

if(CONFLUX_HTTP_CLIENT_STACK_REQUESTED)
add_library(conflux_dns_bridge STATIC)
target_sources(conflux_dns_bridge
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/client_dns_bridge.cxx
)
target_link_libraries(conflux_dns_bridge
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_dns
)

add_library(conflux_net_client STATIC)
target_sources(conflux_net_client
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES
            ${CONFLUX_SRC_ROOT}/net/client_wire.cxx
            ${CONFLUX_SRC_ROOT}/net/client.cxx
            ${CONFLUX_SRC_ROOT}/net/http_client.cxx
)
target_link_libraries(conflux_net_client
    PRIVATE conflux_options
    PUBLIC  conflux_http_parse_helpers
    PUBLIC  conflux_http_core
    PUBLIC  conflux_utils
    PUBLIC  conflux_work
    PUBLIC  conflux_dns_bridge
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_net_client PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_net_client PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_net_client PUBLIC CONFLUX_HAS_TLS=0)
endif()

add_library(conflux_net_async_client STATIC)
target_sources(conflux_net_async_client
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/client_async.cxx
)
target_sources(conflux_net_async_client
    PRIVATE
        ${CONFLUX_SRC_ROOT}/net/client_async_impl.cxx
)
target_link_libraries(conflux_net_async_client
    PRIVATE conflux_options
    PUBLIC  conflux_net_client
    PUBLIC  conflux_http_core
    PUBLIC  conflux_work
    PUBLIC  conflux_work_uring_executor
    PUBLIC  conflux_uring
    PUBLIC  conflux_socket_io
    PUBLIC  conflux_net_cancel
    PUBLIC  conflux_dns_bridge
)
if(TARGET conflux_net_tls)
    target_compile_definitions(conflux_net_async_client PUBLIC CONFLUX_HAS_TLS=1)
    target_link_libraries(conflux_net_async_client PUBLIC conflux_net_tls)
else()
    target_compile_definitions(conflux_net_async_client PUBLIC CONFLUX_HAS_TLS=0)
endif()
endif() # CONFLUX_HTTP_CLIENT_STACK_REQUESTED

if(CONFLUX_EFFECTIVE_SMTP OR (CONFLUX_WANT_HTTP_SERVER AND CONFLUX_HAS_TLS STREQUAL "true"))
add_library(conflux_net_smtp STATIC)
target_sources(conflux_net_smtp
    PUBLIC FILE_SET CXX_MODULES
        BASE_DIRS "${CONFLUX_SRC_ROOT}"
        FILES ${CONFLUX_SRC_ROOT}/net/smtp.cxx
)
target_link_libraries(conflux_net_smtp
    PRIVATE conflux_options
    PUBLIC  conflux_types
    PUBLIC  conflux_crypto
    PUBLIC  conflux_utils
    PUBLIC  conflux_dns
    PUBLIC  conflux_work
    PUBLIC  conflux_net_tls
)
endif() # CONFLUX_EFFECTIVE_SMTP || TLS HTTP server
