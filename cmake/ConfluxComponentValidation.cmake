function(conflux_require_component_flag request_var dependency_var diagnostic)
    if(${request_var} AND NOT ${dependency_var})
        message(FATAL_ERROR "${diagnostic}")
    endif()
endfunction()

set(CONFLUX_EFFECTIVE_FILE_IO_SYNC TRUE)
set(CONFLUX_EFFECTIVE_SMTP "${CONFLUX_WANT_SMTP}")

if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    set(_conflux_json_component_available "${CONFLUX_WANT_JSON}")
else()
    set(_conflux_json_component_available FALSE)
    if(TARGET conflux_json)
        set(_conflux_json_component_available TRUE)
    endif()
endif()

conflux_require_component_flag(CONFLUX_WANT_FILE_WATCH CONFLUX_WANT_FILE_IO
    "conflux: CONFLUX_BUILD_FILE_WATCH requires CONFLUX_BUILD_FILE_IO")
conflux_require_component_flag(CONFLUX_WANT_TEMPLATES _conflux_json_component_available
    "conflux: CONFLUX_BUILD_TEMPLATES requires CONFLUX_BUILD_JSON")
conflux_require_component_flag(CONFLUX_WANT_JSON_FILE _conflux_json_component_available
    "conflux: CONFLUX_BUILD_JSON_FILE requires CONFLUX_BUILD_JSON")
conflux_require_component_flag(CONFLUX_WANT_JSON_FILE CONFLUX_EFFECTIVE_FILE_IO_SYNC
    "conflux: CONFLUX_BUILD_JSON_FILE requires CONFLUX_BUILD_FILE_IO_SYNC")
conflux_require_component_flag(CONFLUX_WANT_TEMPLATES_WATCH CONFLUX_WANT_TEMPLATES
    "conflux: CONFLUX_BUILD_TEMPLATES_WATCH requires CONFLUX_BUILD_TEMPLATES")
conflux_require_component_flag(CONFLUX_WANT_TEMPLATES_WATCH CONFLUX_WANT_FILE_WATCH
    "conflux: CONFLUX_BUILD_TEMPLATES_WATCH requires CONFLUX_BUILD_FILE_WATCH")
if(CONFLUX_WANT_HTTP_JSON OR CONFLUX_WANT_HTTP_SERVER)
    set(_conflux_http_json_or_server_requested TRUE)
else()
    set(_conflux_http_json_or_server_requested FALSE)
endif()
conflux_require_component_flag(_conflux_http_json_or_server_requested CONFLUX_WANT_HTTP_CORE
    "conflux: CONFLUX_BUILD_HTTP_JSON/CONFLUX_BUILD_HTTP_SERVER requires CONFLUX_BUILD_HTTP_CORE")
if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    set(_conflux_json_boundary_component_available "${CONFLUX_WANT_JSON}")
else()
    set(_conflux_json_boundary_component_available FALSE)
    if(TARGET conflux_json_boundary)
        set(_conflux_json_boundary_component_available TRUE)
    endif()
endif()
conflux_require_component_flag(_conflux_http_json_or_server_requested _conflux_json_boundary_component_available
    "conflux: CONFLUX_BUILD_HTTP_JSON/CONFLUX_BUILD_HTTP_SERVER requires conflux_json_boundary")

set(CONFLUX_HTTP_ROUTER_STACK_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_ROUTER OR CONFLUX_WANT_HTTP_SERVER
        OR CONFLUX_WANT_HTTP_POLICY OR CONFLUX_WANT_HTTP_AUTH
        OR CONFLUX_WANT_HTTP_COMPRESSION OR CONFLUX_WANT_HTTP_PROXY
        OR CONFLUX_WANT_HTTP_OBSERVABILITY OR CONFLUX_WANT_HTTP_OPENAPI
        OR CONFLUX_WANT_HTTP_VHOST)
    set(CONFLUX_HTTP_ROUTER_STACK_REQUESTED TRUE)
endif()

set(CONFLUX_HTTP_CLIENT_STACK_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_CLIENT OR CONFLUX_WANT_HTTP_PROXY OR CONFLUX_WANT_HTTP_SERVER)
    set(CONFLUX_HTTP_CLIENT_STACK_REQUESTED TRUE)
endif()

set(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_STATIC OR CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
    set(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED TRUE)
endif()

set(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED FALSE)
if(CONFLUX_WANT_HTTP_REALTIME OR CONFLUX_HTTP_ROUTER_STACK_REQUESTED)
    set(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED TRUE)
endif()

conflux_require_component_flag(CONFLUX_HTTP_STATIC_SURFACE_REQUESTED CONFLUX_WANT_RUNTIME
    "conflux: CONFLUX_BUILD_HTTP_STATIC requires CONFLUX_BUILD_RUNTIME")

conflux_require_component_flag(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED CONFLUX_WANT_HTTP_CORE
    "conflux: CONFLUX_BUILD_HTTP_REALTIME/HTTP router components require CONFLUX_BUILD_HTTP_CORE")
conflux_require_component_flag(CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED CONFLUX_WANT_CRYPTO
    "conflux: CONFLUX_BUILD_HTTP_REALTIME/HTTP router components require CONFLUX_BUILD_CRYPTO")

conflux_require_component_flag(CONFLUX_HTTP_ROUTER_STACK_REQUESTED CONFLUX_WANT_HTTP_CORE
    "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_HTTP_CORE")
conflux_require_component_flag(CONFLUX_HTTP_ROUTER_STACK_REQUESTED CONFLUX_WANT_RUNTIME
    "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_RUNTIME")
conflux_require_component_flag(CONFLUX_HTTP_ROUTER_STACK_REQUESTED CONFLUX_WANT_FILE_IO
    "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_FILE_IO")
conflux_require_component_flag(CONFLUX_HTTP_ROUTER_STACK_REQUESTED CONFLUX_WANT_SOCKET_IO
    "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_SOCKET_IO")
conflux_require_component_flag(CONFLUX_HTTP_ROUTER_STACK_REQUESTED CONFLUX_WANT_CRYPTO
    "conflux: HTTP router/middleware component flags require CONFLUX_BUILD_CRYPTO")

conflux_require_component_flag(CONFLUX_HTTP_CLIENT_STACK_REQUESTED CONFLUX_WANT_HTTP_CORE
    "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_HTTP_CORE")
conflux_require_component_flag(CONFLUX_WANT_HTTP_CORE CONFLUX_WANT_CRYPTO
    "conflux: CONFLUX_BUILD_HTTP_CORE requires CONFLUX_BUILD_CRYPTO")
conflux_require_component_flag(CONFLUX_HTTP_CLIENT_STACK_REQUESTED CONFLUX_WANT_RUNTIME
    "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_RUNTIME")
conflux_require_component_flag(CONFLUX_HTTP_CLIENT_STACK_REQUESTED CONFLUX_WANT_SOCKET_IO
    "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_SOCKET_IO")
conflux_require_component_flag(CONFLUX_HTTP_CLIENT_STACK_REQUESTED CONFLUX_WANT_DNS
    "conflux: HTTP client/proxy component flags require CONFLUX_BUILD_DNS")
if(CONFLUX_WANT_SMTP
        AND CONFLUX_HAS_TLS STREQUAL "false"
        AND CONFLUX_BUILD_SMTP STREQUAL "AUTO")
    set(CONFLUX_EFFECTIVE_SMTP FALSE)
elseif(CONFLUX_WANT_SMTP AND CONFLUX_HAS_TLS STREQUAL "false")
    message(FATAL_ERROR "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_TLS_PROVIDER=AUTO/OPENSSL and OpenSSL")
endif()
if(CONFLUX_HAS_TLS STREQUAL "true" AND (CONFLUX_HTTP_CLIENT_STACK_REQUESTED OR CONFLUX_EFFECTIVE_SMTP)
        AND NOT CONFLUX_WANT_FILE_IO)
    message(FATAL_ERROR "conflux: TLS HTTP client/proxy/SMTP components require CONFLUX_BUILD_FILE_IO")
endif()
conflux_require_component_flag(CONFLUX_EFFECTIVE_SMTP CONFLUX_WANT_RUNTIME
    "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_RUNTIME")
conflux_require_component_flag(CONFLUX_EFFECTIVE_SMTP CONFLUX_WANT_DNS
    "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_DNS")
conflux_require_component_flag(CONFLUX_EFFECTIVE_SMTP CONFLUX_WANT_CRYPTO
    "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_CRYPTO")
conflux_require_component_flag(CONFLUX_EFFECTIVE_SMTP CONFLUX_WANT_SOCKET_IO
    "conflux: CONFLUX_BUILD_SMTP requires CONFLUX_BUILD_SOCKET_IO")

conflux_require_component_flag(CONFLUX_WANT_FILE_MAP CONFLUX_EFFECTIVE_FILE_IO_SYNC
    "conflux: CONFLUX_BUILD_FILE_MAP requires CONFLUX_BUILD_FILE_IO_SYNC")
conflux_require_component_flag(CONFLUX_WANT_FILE_IO CONFLUX_EFFECTIVE_FILE_IO_SYNC
    "conflux: CONFLUX_BUILD_FILE_IO requires CONFLUX_BUILD_FILE_IO_SYNC")
conflux_require_component_flag(CONFLUX_WANT_FILE_IO CONFLUX_WANT_FILE_MAP
    "conflux: CONFLUX_BUILD_FILE_IO requires CONFLUX_BUILD_FILE_MAP")
conflux_require_component_flag(CONFLUX_WANT_TEMPLATES CONFLUX_EFFECTIVE_FILE_IO_SYNC
    "conflux: CONFLUX_BUILD_TEMPLATES requires CONFLUX_BUILD_FILE_IO_SYNC")
conflux_require_component_flag(CONFLUX_WANT_PROCESS CONFLUX_WANT_RUNTIME
    "conflux: CONFLUX_BUILD_PROCESS requires CONFLUX_BUILD_RUNTIME")
