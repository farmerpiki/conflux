set(CONFLUX_COMPONENT_DECLARATIONS
    "conflux_core|core|REQUESTABLE|STABLE"
    "conflux_types|types|REQUESTABLE|STABLE"
    "conflux_utils|utils|REQUESTABLE|STABLE"
    "conflux_net_config|net_config|REQUESTABLE|ADVANCED"
    "conflux_net_cancel|net_cancel|REQUESTABLE|ADVANCED"
    "conflux_net_tls|net_tls|REQUESTABLE|ADVANCED"
    "conflux_process|process|REQUESTABLE|ADVANCED"
    "conflux_net_io_buffer|net_io_buffer|REQUESTABLE|ADVANCED"
    "conflux_crypto|crypto|REQUESTABLE|ADVANCED"
    "conflux_json_boundary|json_boundary|REQUESTABLE|ADVANCED"
    "conflux_json|json|REQUESTABLE|STABLE"
    "conflux_json_native_provider|json_native_provider|REQUESTABLE|ADVANCED"
    "conflux_json_file|json_file|REQUESTABLE|STABLE"
    "conflux_json_reflect|json_reflect|EXPERIMENTAL|EXPERIMENTAL"
    "conflux_json_reflect_provider|json_reflect_provider|EXPERIMENTAL|EXPERIMENTAL"
    "conflux_template|template|REQUESTABLE|ADVANCED"
    "conflux_file_watch|file_watch|REQUESTABLE|ADVANCED"
    "conflux_template_watch|template_watch|REQUESTABLE|ADVANCED"
    "conflux_http_parse_helpers|http_parse_helpers|EXPLICIT|ADVANCED"
    "conflux_http_core|http_core|REQUESTABLE|ADVANCED"
    "conflux_http_router|http_router|REQUESTABLE|ADVANCED"
    "conflux_http_router_dispatch|router_dispatch|EXPLICIT|ADVANCED"
    "conflux_http_router_match|router_match|EXPLICIT|ADVANCED"
    "conflux_http_router_static|router_static|EXPLICIT|ADVANCED"
    "conflux_http_static|http_static|REQUESTABLE|ADVANCED"
    "conflux_http_static_core|http_static_core|EXPLICIT|ADVANCED"
    "conflux_http_static_async|http_static_async|EXPLICIT|ADVANCED"
    "conflux_http_realtime|http_realtime|REQUESTABLE|ADVANCED"
    "conflux_http_response|http_response|REQUESTABLE|ADVANCED"
    "conflux_http_server_helpers|http_server_helpers|EXPLICIT|ADVANCED"
    "conflux_http_server_config|http_server_config|REQUESTABLE|ADVANCED"
    "conflux_http_policy|http_policy|REQUESTABLE|ADVANCED"
    "conflux_http_auth|http_auth|REQUESTABLE|ADVANCED"
    "conflux_http_compression|http_compression|REQUESTABLE|ADVANCED"
    "conflux_net_client|http_client|REQUESTABLE|ADVANCED"
    "conflux_net_async_client|http_async_client|REQUESTABLE|ADVANCED"
    "conflux_http_proxy|http_proxy|REQUESTABLE|ADVANCED"
    "conflux_net_smtp|smtp|REQUESTABLE|ADVANCED"
    "conflux_dns_bridge|dns_bridge|REQUESTABLE|ADVANCED"
    "conflux_http_observability|http_observability|REQUESTABLE|ADVANCED"
    "conflux_http_openapi|http_openapi|REQUESTABLE|ADVANCED"
    "conflux_http_vhost|http_vhost|REQUESTABLE|ADVANCED"
    "conflux_http_json|http_json|REQUESTABLE|ADVANCED"
    "conflux_http_response_json|http_response_json|REQUESTABLE|ADVANCED"
    "conflux_http_app_json|http_app_json|REQUESTABLE|ADVANCED"
    "conflux_http_native_json|http_native_json|REQUESTABLE|ADVANCED"
    "conflux_http1|http1|REQUESTABLE|ADVANCED"
    "conflux_http2|http2|REQUESTABLE|ADVANCED"
    "conflux_http3|http3|EXPERIMENTAL|EXPERIMENTAL"
    "conflux_http_protocol|http_protocol|REQUESTABLE|ADVANCED"
    "conflux_http_server|http_server|REQUESTABLE|ADVANCED"
    "conflux_http_app|http_app|REQUESTABLE|ADVANCED"
    "conflux_net_http|http|REQUESTABLE|STABLE"
    "conflux_uring|uring|REQUESTABLE|ADVANCED"
    "conflux_uring_timeout|uring_timeout|REQUESTABLE|ADVANCED"
    "conflux_work|work|REQUESTABLE|ADVANCED"
    "conflux_file_io_sync|file_io_sync|REQUESTABLE|ADVANCED"
    "conflux_file_map|file_map|REQUESTABLE|ADVANCED"
    "conflux_file_io|file_io|REQUESTABLE|ADVANCED"
    "conflux_socket_io|socket_io|REQUESTABLE|ADVANCED"
    "conflux_dns|dns|REQUESTABLE|ADVANCED"
    "conflux_pg|pg|REQUESTABLE|ADVANCED"
    "conflux|umbrella|REQUESTABLE|STABLE"
    "conflux_options|_options|SUPPORT|INTERNAL_SUPPORT"
    "conflux_direct_slot_pool|_direct_slot_pool|SUPPORT|INTERNAL_SUPPORT"
    "conflux_uring_primitives|_uring_primitives|SUPPORT|INTERNAL_SUPPORT"
    "conflux_simd_runtime|_simd_runtime|SUPPORT|INTERNAL_SUPPORT"
    "conflux_cpu_features|_cpu_features|SUPPORT|INTERNAL_SUPPORT")

set(CONFLUX_INSTALLED_SURFACE_ALIAS_DECLARATIONS
    "http|HTTP_FACADE|VISIBLE"
    "net_config|HTTP_CONFIG|VISIBLE"
    "http_observability|HTTP_METRICS|METRICS"
    "http_auth|TLS_JWT|OPENSSL"
    "template|TEMPLATES|VISIBLE"
    "template_watch|TEMPLATES_WATCH|VISIBLE"
    "pg|DB|VISIBLE")

set(CONFLUX_HEADER_IMPL_DECLARATIONS
    "conflux_header_impl_core|header_impl_core|^conflux\.(types|utils)($|[.:])"
    "conflux_header_impl_json|header_impl_json|^conflux\.json($|[.:])"
    "conflux_header_impl_runtime|header_impl_runtime|^conflux\.(uring($|[.:])|work($|[.:])|net\.io_buffer($|[.:])|net\.cancel($|[.:]))"
    "conflux_header_impl_file_io_sync|header_impl_file_io_sync|^conflux\.file_io_sync$"
    "conflux_header_impl_file_map|header_impl_file_map|^conflux\.file_map$"
    "conflux_header_impl_file_io|header_impl_file_io|^conflux\.file_io($|[.:])"
    "conflux_header_impl_socket_io|header_impl_socket_io|^conflux\.socket_io($|[.:])"
    "conflux_header_impl_dns|header_impl_dns|^conflux\.net\.dns($|[.:])"
    "conflux_header_impl_process|header_impl_process|^conflux\.process($|[.:])"
    "conflux_header_impl_crypto|header_impl_crypto|^conflux\.crypto($|[.:])"
    "conflux_header_impl_http_core|header_impl_http_core|^conflux\.(http($|:problem)|net\.(app($|[.:])|config($|[.:])|http\.types|http\.request|http\.server_types|http\.json|http1|http_parse_helpers|response($|[.:])|router($|[.:])))"
    "conflux_header_impl_http_server|header_impl_http_server|^conflux\.net\.http_server($|[.:])"
    "conflux_header_impl_http_static|header_impl_http_static|^conflux\.net\.(router_static|http\.static_async)($|[.:])"
    "conflux_header_impl_http_client|header_impl_http_client|^conflux\.net\.(async_client|client)($|[.:])"
    "conflux_header_impl_http_proxy|header_impl_http_proxy|^conflux\.net\.proxy($|[.:])"
    "conflux_header_impl_templates|header_impl_templates|^conflux\.templates($|[.:])"
    "conflux_header_impl_pg|header_impl_pg|^conflux\.pg($|[.:])"
    "conflux_header_impl_smtp|header_impl_smtp|^conflux\.net\.smtp($|[.:])")

set(CONFLUX_GENERATED_HEADER_SUPPORT_DECLARATIONS
    "conflux_headers|headers"
    "conflux_header_impl|header_impl")

foreach(_entry IN LISTS CONFLUX_HEADER_IMPL_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    list(APPEND CONFLUX_GENERATED_HEADER_SUPPORT_DECLARATIONS
        "${_target}|${_export_name}")
endforeach()

set(CONFLUX_PUBLIC_COMPONENT_DECLARATIONS)
set(CONFLUX_EXPLICIT_COMPONENT_DECLARATIONS)
set(CONFLUX_EXPERIMENTAL_COMPONENT_DECLARATIONS)
set(CONFLUX_SUPPORT_COMPONENT_DECLARATIONS)
foreach(_entry IN LISTS CONFLUX_COMPONENT_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    list(GET _parts 2 _kind)
    list(GET _parts 3 _tier)
    if(_kind STREQUAL "REQUESTABLE")
        list(APPEND CONFLUX_PUBLIC_COMPONENT_DECLARATIONS
            "${_target}|${_export_name}")
    elseif(_kind STREQUAL "EXPLICIT")
        list(APPEND CONFLUX_EXPLICIT_COMPONENT_DECLARATIONS
            "${_target}|${_export_name}")
    elseif(_kind STREQUAL "EXPERIMENTAL")
        list(APPEND CONFLUX_EXPERIMENTAL_COMPONENT_DECLARATIONS
            "${_target}|${_export_name}")
    elseif(_kind STREQUAL "SUPPORT")
        list(APPEND CONFLUX_SUPPORT_COMPONENT_DECLARATIONS
            "${_target}|${_export_name}")
    else()
        message(FATAL_ERROR
            "conflux: unknown component declaration kind '${_kind}'")
    endif()
endforeach()

function(conflux_component_target_for_export out export_name)
    foreach(_entry IN LISTS CONFLUX_COMPONENT_DECLARATIONS)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _target)
        list(GET _parts 1 _export_name)
        if(_export_name STREQUAL export_name)
            set(${out} "${_target}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "conflux: unknown component export name '${export_name}'")
endfunction()

function(conflux_generated_header_support_target_for_export out export_name)
    foreach(_entry IN LISTS CONFLUX_GENERATED_HEADER_SUPPORT_DECLARATIONS)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _target)
        list(GET _parts 1 _export_name)
        if(_export_name STREQUAL export_name)
            set(${out} "${_target}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "conflux: unknown generated header support export name '${export_name}'")
endfunction()
