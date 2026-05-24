# ---------------------------------------------------------------------------
# Public package targets / install-export
# ---------------------------------------------------------------------------

set(CONFLUX_PACKAGE_COMPONENTS)
set(CONFLUX_PACKAGE_TARGETS)
set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS)
set(CONFLUX_PACKAGE_SUPPORT_TARGETS)
set(CONFLUX_PACKAGE_ALL_COMPONENTS)
set(CONFLUX_PACKAGE_ALL_TARGETS)
set(CONFLUX_PUBLIC_COMPONENT_TARGETS)
set(CONFLUX_INSTALL_TARGET_CANDIDATES)

function(conflux_component target export_name)
    set(options)
    set(one_value_args KIND)
    set(multi_value_args)
    cmake_parse_arguments(CONFLUX_COMPONENT
        "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT CONFLUX_COMPONENT_KIND)
        set(CONFLUX_COMPONENT_KIND REQUESTABLE)
    endif()
    if(NOT CONFLUX_COMPONENT_KIND MATCHES "^(REQUESTABLE|SUPPORT)$")
        message(FATAL_ERROR
            "conflux: component '${export_name}' has invalid KIND '${CONFLUX_COMPONENT_KIND}'")
    endif()

    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
        if(NOT TARGET conflux::${export_name})
            add_library(conflux::${export_name} ALIAS ${target})
        endif()

        set(_all_components ${CONFLUX_PACKAGE_ALL_COMPONENTS})
        list(APPEND _all_components "${export_name}")
        set(CONFLUX_PACKAGE_ALL_COMPONENTS "${_all_components}" PARENT_SCOPE)

        set(_all_targets ${CONFLUX_PACKAGE_ALL_TARGETS})
        list(APPEND _all_targets "conflux::${export_name}")
        set(CONFLUX_PACKAGE_ALL_TARGETS "${_all_targets}" PARENT_SCOPE)

        set(_install_target_candidates ${CONFLUX_INSTALL_TARGET_CANDIDATES})
        list(APPEND _install_target_candidates ${target})
        set(CONFLUX_INSTALL_TARGET_CANDIDATES
            "${_install_target_candidates}" PARENT_SCOPE)

        if(CONFLUX_COMPONENT_KIND STREQUAL "SUPPORT")
            set(_support_components ${CONFLUX_PACKAGE_SUPPORT_COMPONENTS})
            list(APPEND _support_components "${export_name}")
            set(CONFLUX_PACKAGE_SUPPORT_COMPONENTS "${_support_components}" PARENT_SCOPE)

            set(_support_targets ${CONFLUX_PACKAGE_SUPPORT_TARGETS})
            list(APPEND _support_targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_SUPPORT_TARGETS "${_support_targets}" PARENT_SCOPE)
        else()
            set(_components ${CONFLUX_PACKAGE_COMPONENTS})
            list(APPEND _components "${export_name}")
            set(CONFLUX_PACKAGE_COMPONENTS "${_components}" PARENT_SCOPE)

            set(_targets ${CONFLUX_PACKAGE_TARGETS})
            list(APPEND _targets "conflux::${export_name}")
            set(CONFLUX_PACKAGE_TARGETS "${_targets}" PARENT_SCOPE)

            set(_public_targets ${CONFLUX_PUBLIC_COMPONENT_TARGETS})
            list(APPEND _public_targets ${target})
            set(CONFLUX_PUBLIC_COMPONENT_TARGETS
                "${_public_targets}" PARENT_SCOPE)
        endif()

    endif()
endfunction()

macro(conflux_public_component target export_name)
    conflux_component(${target} ${export_name} KIND REQUESTABLE)
endmacro()

function(conflux_target_has_module_file_sets out target)
    set(_has_module_file_sets FALSE)
    foreach(_property IN ITEMS CXX_MODULE_SETS INTERFACE_CXX_MODULE_SETS)
        get_target_property(_sets ${target} ${_property})
        if(_sets)
            set(_has_module_file_sets TRUE)
            break()
        endif()
    endforeach()
    set(${out} ${_has_module_file_sets} PARENT_SCOPE)
endfunction()

conflux_public_component(conflux_core core)
conflux_public_component(conflux_types types)
conflux_public_component(conflux_utils utils)
conflux_public_component(conflux_net_config net_config)
conflux_public_component(conflux_net_cancel net_cancel)
conflux_public_component(conflux_net_tls net_tls)
conflux_public_component(conflux_process process)
conflux_public_component(conflux_net_io_buffer net_io_buffer)
conflux_public_component(conflux_crypto crypto)
conflux_public_component(conflux_json_boundary json_boundary)
conflux_public_component(conflux_json json)
conflux_public_component(conflux_json_native_provider json_native_provider)
conflux_public_component(conflux_json_file json_file)
conflux_public_component(conflux_json_reflect json_reflect)
conflux_public_component(conflux_json_reflect_provider json_reflect_provider)
conflux_public_component(conflux_template template)
conflux_public_component(conflux_file_watch file_watch)
conflux_public_component(conflux_template_watch template_watch)
conflux_public_component(conflux_http_parse_helpers http_parse_helpers)
conflux_public_component(conflux_http_core http_core)
conflux_public_component(conflux_http_router http_router)
conflux_public_component(conflux_http_router_dispatch router_dispatch)
conflux_public_component(conflux_http_router_match router_match)
conflux_public_component(conflux_http_router_static router_static)
conflux_public_component(conflux_http_static http_static)
conflux_public_component(conflux_http_static_core http_static_core)
conflux_public_component(conflux_http_static_async http_static_async)
conflux_public_component(conflux_http_realtime http_realtime)
conflux_public_component(conflux_http_response http_response)
conflux_public_component(conflux_http_server_helpers http_server_helpers)
conflux_public_component(conflux_http_server_config http_server_config)
conflux_public_component(conflux_http_policy http_policy)
conflux_public_component(conflux_http_auth http_auth)
conflux_public_component(conflux_http_compression http_compression)
conflux_public_component(conflux_net_client http_client)
conflux_public_component(conflux_net_async_client http_async_client)
conflux_public_component(conflux_http_proxy http_proxy)
conflux_public_component(conflux_net_smtp smtp)
conflux_public_component(conflux_dns_bridge dns_bridge)
conflux_public_component(conflux_http_observability http_observability)
conflux_public_component(conflux_http_openapi http_openapi)
conflux_public_component(conflux_http_vhost http_vhost)
conflux_public_component(conflux_http_json http_json)
conflux_public_component(conflux_http_response_json http_response_json)
conflux_public_component(conflux_http_app_json http_app_json)
conflux_public_component(conflux_http_native_json http_native_json)
conflux_public_component(conflux_http1 http1)
conflux_public_component(conflux_http2 http2)
conflux_public_component(conflux_http3 http3)
conflux_public_component(conflux_http_protocol http_protocol)
conflux_public_component(conflux_http_server http_server)
conflux_public_component(conflux_http_app http_app)
conflux_public_component(conflux_net_http http)
conflux_public_component(conflux_uring uring)
conflux_public_component(conflux_uring_timeout uring_timeout)
conflux_public_component(conflux_work work)
conflux_public_component(conflux_file_io_sync file_io_sync)
conflux_public_component(conflux_file_map file_map)
conflux_public_component(conflux_file_io file_io)
conflux_public_component(conflux_socket_io socket_io)
conflux_public_component(conflux_dns dns)
conflux_public_component(conflux_db db)
conflux_public_component(conflux_pg pg)
conflux_public_component(conflux umbrella)

# Internal-but-exported support targets needed by static-library link interfaces.
conflux_component(conflux_options _options KIND SUPPORT)
conflux_component(conflux_direct_slot_pool _direct_slot_pool KIND SUPPORT)
conflux_component(conflux_uring_primitives _uring_primitives KIND SUPPORT)
conflux_component(conflux_simd_runtime _simd_runtime KIND SUPPORT)
conflux_component(conflux_cpu_features _cpu_features KIND SUPPORT)

if(TARGET conflux AND NOT TARGET conflux::conflux)
    add_library(conflux::conflux ALIAS conflux)
    list(APPEND CONFLUX_PACKAGE_COMPONENTS conflux)
    list(APPEND CONFLUX_PACKAGE_TARGETS conflux::conflux)
    list(APPEND CONFLUX_PACKAGE_ALL_COMPONENTS conflux)
    list(APPEND CONFLUX_PACKAGE_ALL_TARGETS conflux::conflux)
endif()

set(CONFLUX_INSTALL_INTERFACE_TARGETS)
set(CONFLUX_INSTALL_MODULE_TARGETS)
list(REMOVE_DUPLICATES CONFLUX_INSTALL_TARGET_CANDIDATES)
foreach(_target IN LISTS CONFLUX_INSTALL_TARGET_CANDIDATES)
    if(TARGET ${_target})
        if(_target STREQUAL "conflux")
            continue()
        endif()
        conflux_target_has_module_file_sets(_has_module_file_sets ${_target})
        if(_has_module_file_sets)
            list(APPEND CONFLUX_INSTALL_MODULE_TARGETS ${_target})
        else()
            list(APPEND CONFLUX_INSTALL_INTERFACE_TARGETS ${_target})
        endif()
    endif()
endforeach()

foreach(_target IN LISTS CONFLUX_INSTALL_MODULE_TARGETS)
    if(NOT _target STREQUAL "conflux_types")
        get_target_property(_target_type ${_target} TYPE)
        if(_target_type STREQUAL "INTERFACE_LIBRARY")
            target_link_libraries(${_target} INTERFACE conflux_types)
        else()
            target_link_libraries(${_target} PUBLIC conflux_types)
        endif()
    endif()
endforeach()

if(CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
    set(_conflux_public_module_import_smoke_targets)
    list(REMOVE_DUPLICATES CONFLUX_PUBLIC_COMPONENT_TARGETS)
    foreach(_target IN LISTS CONFLUX_PUBLIC_COMPONENT_TARGETS)
        if(TARGET ${_target})
            conflux_target_has_module_file_sets(_has_module_file_sets ${_target})
            if(_has_module_file_sets)
                list(APPEND _conflux_public_module_import_smoke_targets ${_target})
            endif()
        endif()
    endforeach()
    conflux_add_public_module_import_smoke_target(
        TARGETS ${_conflux_public_module_import_smoke_targets})
    unset(_conflux_public_module_import_smoke_targets)
endif()

if(CONFLUX_INSTALL_INTERFACE_TARGETS)
    foreach(_target IN LISTS CONFLUX_INSTALL_INTERFACE_TARGETS)
        get_target_property(_export_name ${_target} EXPORT_NAME)
        install(TARGETS ${_target}
            EXPORT confluxTargets-${_export_name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endforeach()
endif()

if(CONFLUX_INSTALL_MODULE_TARGETS)
    foreach(_target IN LISTS CONFLUX_INSTALL_MODULE_TARGETS)
        get_target_property(_export_name ${_target} EXPORT_NAME)
        install(TARGETS ${_target}
            EXPORT confluxTargets-${_export_name}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
            FILE_SET generated_mods DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/generated/modules
        )
    endforeach()
endif()

if(TARGET conflux)
    install(TARGETS conflux
        EXPORT confluxTargets-umbrella
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
        FILE_SET generated_mods DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/generated/modules
    )
endif()

set(CONFLUX_INSTALL_MOCK_RUNTIME FALSE)
set(_conflux_install_targets_for_runtime_scan
    ${CONFLUX_INSTALL_INTERFACE_TARGETS}
    ${CONFLUX_INSTALL_MODULE_TARGETS})
if(TARGET conflux)
    list(APPEND _conflux_install_targets_for_runtime_scan conflux)
endif()
conflux_any_target_links_item(CONFLUX_INSTALL_NEEDS_LIBURING
    PkgConfig::LIBURING
    ${_conflux_install_targets_for_runtime_scan})
unset(_conflux_install_targets_for_runtime_scan)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfluxGeneratePackageMetadata.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake"
    @ONLY
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conflux-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/conflux-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
)
install(FILES
    ${CONFLUX_SRC_ROOT}/json_simd_backend.hxx
    ${CONFLUX_SRC_ROOT}/simd_backend.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules/src
)
install(FILES
    ${CONFLUX_SRC_ROOT}/json_simd_backend.hxx
    ${CONFLUX_SRC_ROOT}/simd_backend.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/modules
)
install(FILES
    ${CONFLUX_SRC_ROOT}/conflux/detail/discard.hxx
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/conflux/detail
)
foreach(_target IN LISTS CONFLUX_INSTALL_INTERFACE_TARGETS CONFLUX_INSTALL_MODULE_TARGETS)
    get_target_property(_export_name ${_target} EXPORT_NAME)
    install(EXPORT confluxTargets-${_export_name}
        NAMESPACE conflux::
        FILE confluxTargets-${_export_name}.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endforeach()
if(TARGET conflux)
    install(EXPORT confluxTargets-umbrella
        NAMESPACE conflux::
        FILE confluxTargets-umbrella.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/conflux
    )
endif()
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")
