set(CONFLUX_COMPONENT_DECLARATIONS
    "conflux_core|core|REQUESTABLE"
    "conflux_types|types|REQUESTABLE"
    "conflux_utils|utils|REQUESTABLE"
    "conflux_net_config|net_config|REQUESTABLE"
    "conflux_net_cancel|net_cancel|REQUESTABLE"
    "conflux_net_tls|net_tls|REQUESTABLE"
    "conflux_process|process|REQUESTABLE"
    "conflux_net_io_buffer|net_io_buffer|REQUESTABLE"
    "conflux_crypto|crypto|REQUESTABLE"
    "conflux_json_boundary|json_boundary|REQUESTABLE"
    "conflux_json|json|REQUESTABLE"
    "conflux_json_native_provider|json_native_provider|REQUESTABLE"
    "conflux_json_file|json_file|REQUESTABLE"
    "conflux_json_reflect|json_reflect|REQUESTABLE"
    "conflux_json_reflect_provider|json_reflect_provider|REQUESTABLE"
    "conflux_template|template|REQUESTABLE"
    "conflux_file_watch|file_watch|REQUESTABLE"
    "conflux_template_watch|template_watch|REQUESTABLE"
    "conflux_http_parse_helpers|http_parse_helpers|REQUESTABLE"
    "conflux_http_core|http_core|REQUESTABLE"
    "conflux_http_router|http_router|REQUESTABLE"
    "conflux_http_router_dispatch|router_dispatch|REQUESTABLE"
    "conflux_http_router_match|router_match|REQUESTABLE"
    "conflux_http_router_static|router_static|REQUESTABLE"
    "conflux_http_static|http_static|REQUESTABLE"
    "conflux_http_static_core|http_static_core|REQUESTABLE"
    "conflux_http_static_async|http_static_async|REQUESTABLE"
    "conflux_http_realtime|http_realtime|REQUESTABLE"
    "conflux_http_response|http_response|REQUESTABLE"
    "conflux_http_server_helpers|http_server_helpers|REQUESTABLE"
    "conflux_http_server_config|http_server_config|REQUESTABLE"
    "conflux_http_policy|http_policy|REQUESTABLE"
    "conflux_http_auth|http_auth|REQUESTABLE"
    "conflux_http_compression|http_compression|REQUESTABLE"
    "conflux_net_client|http_client|REQUESTABLE"
    "conflux_net_async_client|http_async_client|REQUESTABLE"
    "conflux_http_proxy|http_proxy|REQUESTABLE"
    "conflux_net_smtp|smtp|REQUESTABLE"
    "conflux_dns_bridge|dns_bridge|REQUESTABLE"
    "conflux_http_observability|http_observability|REQUESTABLE"
    "conflux_http_openapi|http_openapi|REQUESTABLE"
    "conflux_http_vhost|http_vhost|REQUESTABLE"
    "conflux_http_json|http_json|REQUESTABLE"
    "conflux_http_response_json|http_response_json|REQUESTABLE"
    "conflux_http_app_json|http_app_json|REQUESTABLE"
    "conflux_http_native_json|http_native_json|REQUESTABLE"
    "conflux_http1|http1|REQUESTABLE"
    "conflux_http2|http2|REQUESTABLE"
    "conflux_http3|http3|REQUESTABLE"
    "conflux_http_protocol|http_protocol|REQUESTABLE"
    "conflux_http_server|http_server|REQUESTABLE"
    "conflux_http_app|http_app|REQUESTABLE"
    "conflux_net_http|http|REQUESTABLE"
    "conflux_uring|uring|REQUESTABLE"
    "conflux_uring_timeout|uring_timeout|REQUESTABLE"
    "conflux_work|work|REQUESTABLE"
    "conflux_file_io_sync|file_io_sync|REQUESTABLE"
    "conflux_file_map|file_map|REQUESTABLE"
    "conflux_file_io|file_io|REQUESTABLE"
    "conflux_socket_io|socket_io|REQUESTABLE"
    "conflux_dns|dns|REQUESTABLE"
    "conflux_pg|pg|REQUESTABLE"
    "conflux|umbrella|REQUESTABLE"
    "conflux_options|_options|SUPPORT"
    "conflux_direct_slot_pool|_direct_slot_pool|SUPPORT"
    "conflux_uring_primitives|_uring_primitives|SUPPORT"
    "conflux_simd_runtime|_simd_runtime|SUPPORT"
    "conflux_cpu_features|_cpu_features|SUPPORT")

set(CONFLUX_PUBLIC_COMPONENT_DECLARATIONS)
set(CONFLUX_SUPPORT_COMPONENT_DECLARATIONS)
foreach(_entry IN LISTS CONFLUX_COMPONENT_DECLARATIONS)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _target)
    list(GET _parts 1 _export_name)
    list(GET _parts 2 _kind)
    if(_kind STREQUAL "REQUESTABLE")
        list(APPEND CONFLUX_PUBLIC_COMPONENT_DECLARATIONS
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
