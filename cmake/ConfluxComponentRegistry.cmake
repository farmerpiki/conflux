set(CONFLUX_PUBLIC_COMPONENT_DECLARATIONS
    "conflux_core|core"
    "conflux_types|types"
    "conflux_utils|utils"
    "conflux_net_config|net_config"
    "conflux_net_cancel|net_cancel"
    "conflux_net_tls|net_tls"
    "conflux_process|process"
    "conflux_net_io_buffer|net_io_buffer"
    "conflux_crypto|crypto"
    "conflux_json_boundary|json_boundary"
    "conflux_json|json"
    "conflux_json_native_provider|json_native_provider"
    "conflux_json_file|json_file"
    "conflux_json_reflect|json_reflect"
    "conflux_json_reflect_provider|json_reflect_provider"
    "conflux_template|template"
    "conflux_file_watch|file_watch"
    "conflux_template_watch|template_watch"
    "conflux_http_parse_helpers|http_parse_helpers"
    "conflux_http_core|http_core"
    "conflux_http_router|http_router"
    "conflux_http_router_dispatch|router_dispatch"
    "conflux_http_router_match|router_match"
    "conflux_http_router_static|router_static"
    "conflux_http_static|http_static"
    "conflux_http_static_core|http_static_core"
    "conflux_http_static_async|http_static_async"
    "conflux_http_realtime|http_realtime"
    "conflux_http_response|http_response"
    "conflux_http_server_helpers|http_server_helpers"
    "conflux_http_server_config|http_server_config"
    "conflux_http_policy|http_policy"
    "conflux_http_auth|http_auth"
    "conflux_http_compression|http_compression"
    "conflux_net_client|http_client"
    "conflux_net_async_client|http_async_client"
    "conflux_http_proxy|http_proxy"
    "conflux_net_smtp|smtp"
    "conflux_dns_bridge|dns_bridge"
    "conflux_http_observability|http_observability"
    "conflux_http_openapi|http_openapi"
    "conflux_http_vhost|http_vhost"
    "conflux_http_json|http_json"
    "conflux_http_response_json|http_response_json"
    "conflux_http_app_json|http_app_json"
    "conflux_http_native_json|http_native_json"
    "conflux_http1|http1"
    "conflux_http2|http2"
    "conflux_http3|http3"
    "conflux_http_protocol|http_protocol"
    "conflux_http_server|http_server"
    "conflux_http_app|http_app"
    "conflux_net_http|http"
    "conflux_uring|uring"
    "conflux_uring_timeout|uring_timeout"
    "conflux_work|work"
    "conflux_file_io_sync|file_io_sync"
    "conflux_file_map|file_map"
    "conflux_file_io|file_io"
    "conflux_socket_io|socket_io"
    "conflux_dns|dns"
    "conflux_pg|pg"
    "conflux|umbrella")

set(CONFLUX_SUPPORT_COMPONENT_DECLARATIONS
    "conflux_options|_options"
    "conflux_direct_slot_pool|_direct_slot_pool"
    "conflux_uring_primitives|_uring_primitives"
    "conflux_simd_runtime|_simd_runtime"
    "conflux_cpu_features|_cpu_features")

function(conflux_component_target_for_export out export_name)
    foreach(_entry IN LISTS
            CONFLUX_PUBLIC_COMPONENT_DECLARATIONS
            CONFLUX_SUPPORT_COMPONENT_DECLARATIONS)
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
