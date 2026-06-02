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
