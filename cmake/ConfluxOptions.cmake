option(CONFLUX_JSON_REFLECT   "Enable P2996 reflection codec (requires a reflection-capable compiler)" OFF)
set(CONFLUX_INTERFACE_MODE "MODULE_INTERFACE" CACHE STRING
    "Public consumer interface: MODULE_INTERFACE or HEADER_INTERFACE")
set_property(CACHE CONFLUX_INTERFACE_MODE PROPERTY STRINGS MODULE_INTERFACE HEADER_INTERFACE)
set(CONFLUX_API_SURFACE "curated" CACHE STRING
    "Aggregate API surface re-exported by import conflux / <conflux.hxx>: curated, extended, or complete")
set_property(CACHE CONFLUX_API_SURFACE PROPERTY STRINGS curated extended complete)
set(CONFLUX_USE_IMPORT_STD "AUTO" CACHE STRING
    "Use the standard-library module in MODULE_INTERFACE mode: AUTO, ON, or OFF")
set_property(CACHE CONFLUX_USE_IMPORT_STD PROPERTY STRINGS AUTO ON OFF)
option(CONFLUX_ROUTER_LAZY_ROUTE_METADATA
    "Skip internal route-pattern param injection unless route observation requests it" ON)
option(CONFLUX_HEADER_INTERFACE_WITH_SOURCES
    "Attach generated non-module implementation sources to conflux::headers; experimental" OFF)
option(CONFLUX_HEADER_FAST_COMPILE
    "Compile generated header-mode implementation and smoke targets without optimization or module scanning" ON)
option(CONFLUX_HEADER_USE_IMPORT_STD
    "Use import std inside generated header-mode compatibility headers" OFF)
option(CONFLUX_HEADER_USE_IMPORT_STD_COMPAT
    "Use import std.compat inside generated header-mode compatibility headers" OFF)
option(CONFLUX_HEADER_USE_MODULE_IMPORTS
    "Use module imports instead of includes for generated header-mode component dependencies" OFF)
option(CONFLUX_HEADER_LINK_EXAMPLES
    "Link generated header-mode example targets against generated implementation archives" OFF)
option(CONFLUX_HEADER_LINK_SMOKE
    "Build small linked header-mode smoke targets against generated implementation archives" OFF)
option(CONFLUX_RUN_HEADER_COMPONENT_SMOKE
    "Run the full generated public header component smoke matrix in CTest" OFF)

if(NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
		AND NOT CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
	message(FATAL_ERROR
		"CONFLUX_INTERFACE_MODE must be MODULE_INTERFACE or HEADER_INTERFACE; got '${CONFLUX_INTERFACE_MODE}'")
endif()
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    set(_conflux_supported_module_toolchain OFF)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17")
        set(_conflux_supported_module_toolchain ON)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "21"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "22")
        set(_conflux_supported_module_toolchain ON)
    endif()
    if(NOT _conflux_supported_module_toolchain)
        message(FATAL_ERROR
            "conflux: MODULE_INTERFACE preview support is limited to the checked "
            "GCC 15, GCC 16, and Clang 21 lanes; got "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. "
            "Use HEADER_INTERFACE for best-effort generated headers, or configure "
            "one of the checked module presets.")
    endif()
endif()
set(_conflux_api_surface_input "${CONFLUX_API_SURFACE}")
string(TOLOWER "${CONFLUX_API_SURFACE}" CONFLUX_API_SURFACE)
if(NOT CONFLUX_API_SURFACE MATCHES "^(curated|extended|complete)$")
    message(FATAL_ERROR
        "CONFLUX_API_SURFACE must be one of: curated, extended, complete; "
        "got '${_conflux_api_surface_input}'")
endif()
set(CONFLUX_API_SURFACE "${CONFLUX_API_SURFACE}" CACHE STRING
    "Aggregate API surface re-exported by import conflux / <conflux.hxx>: curated, extended, or complete" FORCE)
set_property(CACHE CONFLUX_API_SURFACE PROPERTY STRINGS curated extended complete)
set(CONFLUX_API_SURFACE_LEVEL_VALUE 1)
if(CONFLUX_API_SURFACE STREQUAL "extended")
    set(CONFLUX_API_SURFACE_LEVEL_VALUE 2)
elseif(CONFLUX_API_SURFACE STREQUAL "complete")
    set(CONFLUX_API_SURFACE_LEVEL_VALUE 3)
endif()

function(conflux_set_api_surface_presence suffix module_expr header_expr)
    if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
        set(_value "${header_expr}")
    else()
        set(_value "${module_expr}")
    endif()
    string(TOUPPER "${suffix}" _macro_suffix)
    set(_definitions ${_conflux_surface_definitions})
    list(APPEND _definitions
        "$<BUILD_INTERFACE:CONFLUX_SURFACE_HAS_${_macro_suffix}=${_value}>")
    set(_conflux_surface_definitions "${_definitions}" PARENT_SCOPE)
endfunction()

function(conflux_apply_api_surface_definitions target scope)
    set(_conflux_surface_definitions)
    conflux_set_api_surface_presence(types "$<TARGET_EXISTS:conflux_types>" 1)
    conflux_set_api_surface_presence(http_facade "$<TARGET_EXISTS:conflux_net_http>" "$<BOOL:${CONFLUX_WANT_HTTP_CORE}>")
    conflux_set_api_surface_presence(http_app "$<TARGET_EXISTS:conflux_http_app>" "$<BOOL:${CONFLUX_HTTP_ROUTER_STACK_REQUESTED}>")
    conflux_set_api_surface_presence(http_config "$<TARGET_EXISTS:conflux_net_config>" "$<BOOL:${CONFLUX_WANT_HTTP_CORE}>")
    conflux_set_api_surface_presence(http_client "$<TARGET_EXISTS:conflux_net_client>" "$<BOOL:${CONFLUX_WANT_HTTP_CLIENT}>")
    conflux_set_api_surface_presence(http_async_client "$<TARGET_EXISTS:conflux_net_async_client>" "$<BOOL:${CONFLUX_WANT_HTTP_CLIENT}>")
    conflux_set_api_surface_presence(http_auth "$<TARGET_EXISTS:conflux_http_auth>" "$<BOOL:${CONFLUX_WANT_HTTP_AUTH}>")
    conflux_set_api_surface_presence(http_policy "$<TARGET_EXISTS:conflux_http_policy>" "$<BOOL:${CONFLUX_WANT_HTTP_POLICY}>")
    conflux_set_api_surface_presence(http_observability "$<TARGET_EXISTS:conflux_http_observability>" "$<BOOL:${CONFLUX_WANT_HTTP_OBSERVABILITY}>")
    conflux_set_api_surface_presence(http_openapi "$<TARGET_EXISTS:conflux_http_openapi>" "$<BOOL:${CONFLUX_WANT_HTTP_OPENAPI}>")
    conflux_set_api_surface_presence(http_vhost "$<TARGET_EXISTS:conflux_http_vhost>" "$<BOOL:${CONFLUX_WANT_HTTP_VHOST}>")
    conflux_set_api_surface_presence(http_metrics "$<AND:$<TARGET_EXISTS:conflux_http_observability>,$<BOOL:${CONFLUX_HAS_METRICS}>>" "$<AND:$<BOOL:${CONFLUX_WANT_HTTP_OBSERVABILITY}>,$<BOOL:${CONFLUX_HAS_METRICS}>>")
    conflux_set_api_surface_presence(http_protocol "$<TARGET_EXISTS:conflux_http_protocol>" "$<BOOL:${CONFLUX_WANT_HTTP_CORE}>")
    conflux_set_api_surface_presence(http_parse_helpers "$<TARGET_EXISTS:conflux_http_parse_helpers>" "$<BOOL:${CONFLUX_WANT_HTTP_CORE}>")
    conflux_set_api_surface_presence(http_core "$<TARGET_EXISTS:conflux_http_core>" "$<BOOL:${CONFLUX_WANT_HTTP_CORE}>")
    conflux_set_api_surface_presence(http_response "$<TARGET_EXISTS:conflux_http_response>" "$<BOOL:${CONFLUX_WANT_HTTP_CORE}>")
    conflux_set_api_surface_presence(http_json "$<TARGET_EXISTS:conflux_http_json>" "$<BOOL:${CONFLUX_WANT_HTTP_JSON}>")
    conflux_set_api_surface_presence(http_response_json "$<TARGET_EXISTS:conflux_http_response_json>" "$<BOOL:${CONFLUX_WANT_HTTP_JSON}>")
    conflux_set_api_surface_presence(http_app_json "$<TARGET_EXISTS:conflux_http_app_json>" "$<BOOL:${CONFLUX_WANT_HTTP_JSON}>")
    conflux_set_api_surface_presence(http_native_json "$<TARGET_EXISTS:conflux_http_native_json>" "$<BOOL:${CONFLUX_WANT_HTTP_JSON}>")
    conflux_set_api_surface_presence(http_router "$<TARGET_EXISTS:conflux_http_router>" "$<BOOL:${CONFLUX_HTTP_ROUTER_STACK_REQUESTED}>")
    conflux_set_api_surface_presence(http_static "$<TARGET_EXISTS:conflux_http_static>" "$<BOOL:${CONFLUX_HTTP_STATIC_SURFACE_REQUESTED}>")
    conflux_set_api_surface_presence(http_realtime "$<TARGET_EXISTS:conflux_http_realtime>" "$<BOOL:${CONFLUX_HTTP_REALTIME_SURFACE_REQUESTED}>")
    conflux_set_api_surface_presence(http_server "$<TARGET_EXISTS:conflux_http_server>" "$<BOOL:${CONFLUX_WANT_HTTP_SERVER}>")
    conflux_set_api_surface_presence(http_compression "$<TARGET_EXISTS:conflux_http_compression>" "$<BOOL:${CONFLUX_WANT_HTTP_COMPRESSION}>")
    conflux_set_api_surface_presence(http_proxy "$<TARGET_EXISTS:conflux_http_proxy>" "$<BOOL:${CONFLUX_WANT_HTTP_PROXY}>")
    conflux_set_api_surface_presence(http2 "$<TARGET_EXISTS:conflux_http2>" "$<BOOL:${CONFLUX_WANT_HTTP_SERVER}>")
    conflux_set_api_surface_presence(http3 "$<TARGET_EXISTS:conflux_http3>" "$<BOOL:${CONFLUX_HAS_HTTP3}>")
    conflux_set_api_surface_presence(json "$<TARGET_EXISTS:conflux_json>" "$<BOOL:${CONFLUX_WANT_JSON}>")
    conflux_set_api_surface_presence(json_boundary "$<TARGET_EXISTS:conflux_json_boundary>" "$<BOOL:${CONFLUX_WANT_JSON}>")
    conflux_set_api_surface_presence(json_native_provider "$<TARGET_EXISTS:conflux_json_native_provider>" "$<BOOL:${CONFLUX_WANT_JSON}>")
    conflux_set_api_surface_presence(json_file "$<TARGET_EXISTS:conflux_json_file>" "$<BOOL:${CONFLUX_WANT_JSON_FILE}>")
    conflux_set_api_surface_presence(json_reflect "$<TARGET_EXISTS:conflux_json_reflect>" "$<BOOL:${CONFLUX_JSON_REFLECT}>")
    conflux_set_api_surface_presence(json_reflect_provider "$<TARGET_EXISTS:conflux_json_reflect_provider>" "$<BOOL:${CONFLUX_JSON_REFLECT}>")
    conflux_set_api_surface_presence(work "$<TARGET_EXISTS:conflux_work>" "$<BOOL:${CONFLUX_WANT_RUNTIME}>")
    conflux_set_api_surface_presence(uring "$<TARGET_EXISTS:conflux_uring>" "$<BOOL:${CONFLUX_WANT_RUNTIME}>")
    conflux_set_api_surface_presence(uring_timeout "$<TARGET_EXISTS:conflux_uring_timeout>" "$<BOOL:${CONFLUX_WANT_RUNTIME}>")
    conflux_set_api_surface_presence(file_io_sync "$<TARGET_EXISTS:conflux_file_io_sync>" "$<BOOL:${CONFLUX_WANT_FILE_IO_SYNC}>")
    conflux_set_api_surface_presence(file_map "$<TARGET_EXISTS:conflux_file_map>" "$<BOOL:${CONFLUX_WANT_FILE_MAP}>")
    conflux_set_api_surface_presence(file_io "$<TARGET_EXISTS:conflux_file_io>" "$<BOOL:${CONFLUX_WANT_FILE_IO}>")
    conflux_set_api_surface_presence(file_watch "$<TARGET_EXISTS:conflux_file_watch>" "$<BOOL:${CONFLUX_WANT_FILE_WATCH}>")
    conflux_set_api_surface_presence(socket_io "$<TARGET_EXISTS:conflux_socket_io>" "$<BOOL:${CONFLUX_WANT_SOCKET_IO}>")
    conflux_set_api_surface_presence(dns "$<TARGET_EXISTS:conflux_dns>" "$<BOOL:${CONFLUX_WANT_DNS}>")
    conflux_set_api_surface_presence(dns_bridge "$<TARGET_EXISTS:conflux_dns_bridge>" "$<BOOL:${CONFLUX_WANT_DNS}>")
    conflux_set_api_surface_presence(net_io_buffer "$<TARGET_EXISTS:conflux_net_io_buffer>" "$<BOOL:${CONFLUX_WANT_RUNTIME}>")
    conflux_set_api_surface_presence(net_cancel "$<TARGET_EXISTS:conflux_net_cancel>" "$<BOOL:${CONFLUX_WANT_RUNTIME}>")
    conflux_set_api_surface_presence(crypto "$<TARGET_EXISTS:conflux_crypto>" "$<BOOL:${CONFLUX_WANT_CRYPTO}>")
    conflux_set_api_surface_presence(tls_jwt "$<AND:$<TARGET_EXISTS:conflux_http_auth>,$<BOOL:${CONFLUX_HAS_TLS}>>" "$<AND:$<BOOL:${CONFLUX_WANT_HTTP_AUTH}>,$<BOOL:${CONFLUX_HAS_TLS}>>")
    conflux_set_api_surface_presence(templates "$<TARGET_EXISTS:conflux_template>" "$<BOOL:${CONFLUX_WANT_TEMPLATES}>")
    conflux_set_api_surface_presence(templates_watch "$<TARGET_EXISTS:conflux_template_watch>" "$<BOOL:${CONFLUX_WANT_TEMPLATES_WATCH}>")
    conflux_set_api_surface_presence(process "$<TARGET_EXISTS:conflux_process>" "$<BOOL:${CONFLUX_WANT_PROCESS}>")
    conflux_set_api_surface_presence(db "$<TARGET_EXISTS:conflux_pg>" "$<BOOL:${CONFLUX_HAS_DB}>")
    conflux_set_api_surface_presence(smtp "$<TARGET_EXISTS:conflux_net_smtp>" "$<BOOL:${CONFLUX_WANT_SMTP}>")

    target_compile_definitions(${target} ${scope}
        CONFLUX_API_SURFACE_LEVEL=${CONFLUX_API_SURFACE_LEVEL_VALUE}
        CONFLUX_API_SURFACE_CURATED=1
        CONFLUX_API_SURFACE_EXTENDED=2
        CONFLUX_API_SURFACE_COMPLETE=3
        CONFLUX_BUILD_API_SURFACE="${CONFLUX_API_SURFACE}"
        $<BUILD_INTERFACE:CONFLUX_SURFACE_HAS_FEATURES=1>
        ${_conflux_surface_definitions})
endfunction()

function(conflux_target_links_item out target needle)
    if(NOT TARGET ${target})
        set(${out} FALSE PARENT_SCOPE)
        return()
    endif()

    get_property(_visited GLOBAL PROPERTY CONFLUX_LINK_ITEM_SCAN_VISITED)
    if(target IN_LIST _visited)
        set(${out} FALSE PARENT_SCOPE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY CONFLUX_LINK_ITEM_SCAN_VISITED ${target})

    foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_items ${target} ${_property})
        if(NOT _items)
            continue()
        endif()
        foreach(_item IN LISTS _items)
            if(_item MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
                set(_item "${CMAKE_MATCH_1}")
            endif()
            if(_item STREQUAL "${needle}")
                set(${out} TRUE PARENT_SCOPE)
                return()
            endif()
            if(TARGET ${_item})
                conflux_target_links_item(_nested "${_item}" "${needle}")
                if(_nested)
                    set(${out} TRUE PARENT_SCOPE)
                    return()
                endif()
            endif()
        endforeach()
    endforeach()

    set(${out} FALSE PARENT_SCOPE)
endfunction()

function(conflux_any_target_links_item out needle)
    set_property(GLOBAL PROPERTY CONFLUX_LINK_ITEM_SCAN_VISITED "")
    foreach(_target IN LISTS ARGN)
        conflux_target_links_item(_found "${_target}" "${needle}")
        if(_found)
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} FALSE PARENT_SCOPE)
endfunction()
if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    if(CONFLUX_HEADER_USE_IMPORT_STD OR CONFLUX_HEADER_USE_IMPORT_STD_COMPAT OR CONFLUX_HEADER_USE_MODULE_IMPORTS)
        set(CMAKE_CXX_SCAN_FOR_MODULES ON)
    else()
        set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    endif()
endif()

if(CONFLUX_JSON_REFLECT)
    set(CMAKE_CXX_STANDARD 26)
else()
    set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CONFLUX_REFLECTION_COMPILE_OPTIONS "-freflection" CACHE STRING
    "Compiler options required for P2996 reflection-enabled translation units")
if(CONFLUX_JSON_REFLECT AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
        AND CONFLUX_REFLECTION_COMPILE_OPTIONS STREQUAL "-freflection")
    set(CONFLUX_REFLECTION_COMPILE_OPTIONS "-freflection-latest" CACHE STRING
        "Compiler options required for P2996 reflection-enabled translation units" FORCE)
endif()

if(NOT CONFLUX_USE_IMPORT_STD STREQUAL "AUTO"
        AND NOT CONFLUX_USE_IMPORT_STD STREQUAL "ON"
        AND NOT CONFLUX_USE_IMPORT_STD STREQUAL "OFF")
    message(FATAL_ERROR
        "CONFLUX_USE_IMPORT_STD must be AUTO, ON, or OFF; got '${CONFLUX_USE_IMPORT_STD}'")
endif()

set(CONFLUX_IMPORT_STD_ENABLED OFF)
set(_conflux_import_std_supported OFF)
if(${CMAKE_CXX_STANDARD} IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set(_conflux_import_std_supported ON)
endif()
if(_conflux_import_std_supported
        AND CMAKE_CXX_STDLIB_MODULES_JSON
        AND NOT EXISTS "${CMAKE_CXX_STDLIB_MODULES_JSON}")
    if(CONFLUX_USE_IMPORT_STD STREQUAL "ON")
        message(FATAL_ERROR
            "CONFLUX_USE_IMPORT_STD=ON requires CMake's C++ standard library module metadata, "
            "but CMAKE_CXX_STDLIB_MODULES_JSON does not exist: ${CMAKE_CXX_STDLIB_MODULES_JSON}")
    endif()
    set(_conflux_import_std_supported OFF)
endif()

if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    if(CONFLUX_USE_IMPORT_STD STREQUAL "ON" AND NOT _conflux_import_std_supported)
        set(_conflux_import_std_levels "${CMAKE_CXX_COMPILER_IMPORT_STD}")
        if(_conflux_import_std_levels STREQUAL "")
            set(_conflux_import_std_levels "<none>")
        endif()
        message(FATAL_ERROR
            "CONFLUX_USE_IMPORT_STD=ON requires CMake-discoverable C++${CMAKE_CXX_STANDARD} import std support. "
            "Supported import std standard levels for this toolchain: ${_conflux_import_std_levels}. "
            "Use CONFLUX_USE_IMPORT_STD=OFF to keep MODULE_INTERFACE with generated standard-header overlays.")
    elseif(CONFLUX_USE_IMPORT_STD STREQUAL "AUTO" AND _conflux_import_std_supported)
        set(CONFLUX_IMPORT_STD_ENABLED ON)
    elseif(CONFLUX_USE_IMPORT_STD STREQUAL "ON")
        set(CONFLUX_IMPORT_STD_ENABLED ON)
    endif()
elseif(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE"
        AND (CONFLUX_HEADER_USE_IMPORT_STD OR CONFLUX_HEADER_USE_IMPORT_STD_COMPAT))
    if(NOT _conflux_import_std_supported)
        set(_conflux_import_std_levels "${CMAKE_CXX_COMPILER_IMPORT_STD}")
        if(_conflux_import_std_levels STREQUAL "")
            set(_conflux_import_std_levels "<none>")
        endif()
        message(FATAL_ERROR
            "CONFLUX_HEADER_USE_IMPORT_STD=ON or CONFLUX_HEADER_USE_IMPORT_STD_COMPAT=ON "
            "requires CMake-discoverable C++${CMAKE_CXX_STANDARD} import std support. "
            "Supported import std standard levels for this toolchain: ${_conflux_import_std_levels}.")
    endif()
    set(CONFLUX_IMPORT_STD_ENABLED ON)
endif()

if(CONFLUX_IMPORT_STD_ENABLED)
    set(CMAKE_CXX_MODULE_STD ON)
    if(CONFLUX_JSON_REFLECT AND CMAKE_CXX_STDLIB_MODULES_JSON AND EXISTS "${CMAKE_CXX_STDLIB_MODULES_JSON}")
        set(_conflux_apply_reflection_options ON)
    else()
        set(_conflux_apply_reflection_options OFF)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_STDLIB_MODULES_JSON AND EXISTS "${CMAKE_CXX_STDLIB_MODULES_JSON}")
        set(_conflux_apply_clang_std_module_options ON)
    else()
        set(_conflux_apply_clang_std_module_options OFF)
    endif()

    if(_conflux_apply_reflection_options OR _conflux_apply_clang_std_module_options)
        set(_conflux_std_module_options)
        if(_conflux_apply_reflection_options)
            list(APPEND _conflux_std_module_options
                REFLECTION_OPTIONS "${CONFLUX_REFLECTION_COMPILE_OPTIONS}")
        endif()
        if(_conflux_apply_clang_std_module_options)
            list(APPEND _conflux_std_module_options
                SUPPRESS_CLANG_RESERVED_MODULE_IDENTIFIER_WARNING)
        endif()
        conflux_configure_std_module_sources(${_conflux_std_module_options})
    endif()
else()
    set(CMAKE_CXX_MODULE_STD OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(CONFLUX_ENABLE_ASAN    "Build with AddressSanitizer"                  OFF)
option(CONFLUX_ENABLE_UBSAN   "Build with UBSanitizer"                       OFF)
option(CONFLUX_ENABLE_TSAN    "Build with ThreadSanitizer"                   OFF)
option(CONFLUX_ENABLE_LTO     "Enable link-time optimisation"                OFF)
set(CONFLUX_LTO_MODE "AUTO" CACHE STRING "LTO mode when CONFLUX_ENABLE_LTO=ON: AUTO, THIN, or FULL")
set_property(CACHE CONFLUX_LTO_MODE PROPERTY STRINGS AUTO THIN FULL)
if(CONFLUX_JSON_REFLECT
        AND CONFLUX_USE_IMPORT_STD STREQUAL "ON"
        AND NOT CONFLUX_IMPORT_STD_ENABLED)
    set(_conflux_import_std_levels "${CMAKE_CXX_COMPILER_IMPORT_STD}")
    if(_conflux_import_std_levels STREQUAL "")
        set(_conflux_import_std_levels "<none>")
    endif()
    message(FATAL_ERROR
        "conflux: CONFLUX_JSON_REFLECT with CONFLUX_USE_IMPORT_STD=ON requires C++26 import std support. "
        "Supported import std standard levels for this toolchain: ${_conflux_import_std_levels}.")
endif()
option(CONFLUX_PGO_GENERATE  "Instrument build for PGO profile collection"  OFF)
set(CONFLUX_PGO_PROFILE_DIR  "" CACHE STRING "Profile data path for PGO use phase")
option(CONFLUX_BUILD_TESTS    "Build test targets"                           OFF)
option(CONFLUX_BUILD_PACKAGE_TESTS
    "Register package/install-tree CTest smoke tests without building the Catch2 test suite" OFF)
set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated installed conflux components to validate with package smoke tests")
set(CONFLUX_PACKAGE_SMOKE_PREFIX "" CACHE PATH
    "Optional installed conflux prefix to validate with the package smoke project")
set(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE "" CACHE STRING
    "Expected installed interface mode for package smoke tests, or empty to accept the package mode")
option(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER
    "Enable downstream package smoke with one module-import TU and one header-include TU" OFF)
set_property(CACHE CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE PROPERTY STRINGS
    "" MODULE_INTERFACE HEADER_INTERFACE)

function(conflux_escape_package_smoke_components out_var)
    string(REPLACE ";" "\\;" _conflux_escaped_components
        "${CONFLUX_PACKAGE_SMOKE_COMPONENTS}")
    set(${out_var} "${_conflux_escaped_components}" PARENT_SCOPE)
endfunction()

function(conflux_add_package_config_install_tree_test source_dir build_dir)
    conflux_escape_package_smoke_components(_conflux_package_smoke_components)
    set(_conflux_package_smoke_args
        --source "${source_dir}"
        --prefix "${CONFLUX_PACKAGE_SMOKE_PREFIX}"
        --build-dir "${build_dir}"
        --components "${_conflux_package_smoke_components}")
    if(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE)
        list(APPEND _conflux_package_smoke_args
            --interface-mode "${CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE}")
    endif()
    if(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER)
        list(APPEND _conflux_package_smoke_args --mixed-module-header)
    endif()

    add_test(NAME build/package-config-install-tree
        COMMAND "${source_dir}/scripts/run-package-config-smoke.sh"
                ${_conflux_package_smoke_args})
    set_tests_properties(build/package-config-install-tree PROPERTIES
        LABELS "build;package;install")
endfunction()

option(CONFLUX_RUN_INSTALL_TREE_SMOKE
    "Add an opt-in CTest that builds, installs, and consumes a fresh conflux install tree" OFF)
set(CONFLUX_INSTALL_TREE_SMOKE_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/install-tree-smoke-build" CACHE PATH
    "Build directory used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/install-tree-smoke-prefix" CACHE PATH
    "Install prefix used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_CONSUMER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/install-tree-smoke-consumer" CACHE PATH
    "Downstream consumer build directory used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_FEATURE_SET "core" CACHE STRING
    "Feature preset used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_BUILD_TYPE "Release" CACHE STRING
    "CMake build type used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_GENERATOR "${CMAKE_GENERATOR}" CACHE STRING
    "CMake generator used by the opt-in install-tree smoke test")
set(CONFLUX_INSTALL_TREE_SMOKE_INTERFACE_MODE "${CONFLUX_INTERFACE_MODE}" CACHE STRING
    "Interface mode used by the opt-in install-tree smoke test")
set_property(CACHE CONFLUX_INSTALL_TREE_SMOKE_INTERFACE_MODE PROPERTY STRINGS
    MODULE_INTERFACE HEADER_INTERFACE)
set(CONFLUX_INSTALL_TREE_SMOKE_EXTRA_CMAKE_ARGS "" CACHE STRING
    "Semicolon-separated extra CMake configure arguments for the opt-in install-tree smoke test")

function(conflux_add_install_tree_smoke_test source_dir)
    conflux_escape_package_smoke_components(_conflux_package_smoke_components)
    set(_conflux_install_tree_smoke_args
        --source "${source_dir}"
        --build-dir "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_DIR}"
        --prefix "${CONFLUX_INSTALL_TREE_SMOKE_PREFIX}"
        --smoke-build-dir "${CONFLUX_INSTALL_TREE_SMOKE_CONSUMER_BUILD_DIR}"
        --feature-set "${CONFLUX_INSTALL_TREE_SMOKE_FEATURE_SET}"
        --build-type "${CONFLUX_INSTALL_TREE_SMOKE_BUILD_TYPE}"
        --generator "${CONFLUX_INSTALL_TREE_SMOKE_GENERATOR}"
        --interface-mode "${CONFLUX_INSTALL_TREE_SMOKE_INTERFACE_MODE}"
        --components "${_conflux_package_smoke_components}")
    if(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER)
        list(APPEND _conflux_install_tree_smoke_args --mixed-module-header-smoke)
    endif()
    if(CONFLUX_INSTALL_TREE_SMOKE_EXTRA_CMAKE_ARGS)
        list(APPEND _conflux_install_tree_smoke_args
            --
            ${CONFLUX_INSTALL_TREE_SMOKE_EXTRA_CMAKE_ARGS})
    endif()

    add_test(NAME build/install-tree-smoke
        COMMAND "${source_dir}/scripts/run-install-tree-smoke.sh"
                ${_conflux_install_tree_smoke_args})
    set_tests_properties(build/install-tree-smoke PROPERTIES
        LABELS "build;package;install")
endfunction()
option(CONFLUX_FETCH_TEST_DEPS "Allow FetchContent downloads for test-only dependencies" OFF)
set(CONFLUX_TEST_CATCH2_PROVIDER "SYSTEM" CACHE STRING
    "Catch2 provider for tests: FETCH builds Catch2 with the active stdlib, SYSTEM uses find_package, AUTO tries SYSTEM then FETCH")
set_property(CACHE CONFLUX_TEST_CATCH2_PROVIDER PROPERTY STRINGS FETCH SYSTEM AUTO)
set(CONFLUX_CATCH2_SOURCE_DIR "" CACHE PATH "Path to a local Catch2 source tree for CONFLUX_TEST_CATCH2_PROVIDER=FETCH")
option(CONFLUX_ENABLE_JSON_TESTSUITE "Build nst/JSONTestSuite conformance gate when JSONTestSuite is available" OFF)
option(CONFLUX_ENABLE_THIRD_PARTY_TESTS
    "Register optional third-party protocol conformance tests such as h2spec and Autobahn" OFF)
set(JSONTESTSUITE_DIR "" CACHE PATH "Path to nst/JSONTestSuite/test_parsing, or repository root containing test_parsing")
option(CONFLUX_BUILD_BENCHMARKS "Build benchmark targets"                    OFF)
option(CONFLUX_BUILD_EXAMPLES "Build example targets"                        OFF)
option(CONFLUX_ALLOW_SANITIZED_BENCHMARKS
    "Allow benchmark targets in sanitizer builds for local instrumentation debugging only" OFF)
option(CONFLUX_BUILD_FUZZ     "Build libFuzzer harness targets"              OFF)
option(CONFLUX_BUILD_FUZZ_SMOKE_TESTS
    "Register bounded CTest smoke tests for libFuzzer harnesses" ON)
option(CONFLUX_ENABLE_METRICS "Build Prometheus-compatible metrics support"  ON)
option(CONFLUX_ENABLE_EXPERIMENTAL "Enable experimental runtime features and their tests/benchmarks" OFF)
option(CONFLUX_ENABLE_RECV_BUNDLE "Allow io_uring multishot recv bundling when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_RECV_INCREMENTAL_BUF "Allow experimental incremental provided-buffer rings when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_SEND_ZC "Allow experimental io_uring SEND_ZC when configured and supported by the kernel" ON)
option(CONFLUX_ENABLE_RING_GROWTH "Allow experimental io_uring CQ ring growth after overflow" ON)
option(CONFLUX_ENABLE_IOPOLL_STORAGE_TEST "Allow experimental IOPOLL/O_DIRECT storage-ring test coverage" ON)
option(CONFLUX_ENABLE_HTTP_TRACE "Enable debug-only HTTP/server tracing"     OFF)
option(CONFLUX_WORK_CARRIER_MODEL_A "Enable experimental work carrier Model A prototype surface" ON)
option(CONFLUX_WORK_CARRIER_MODEL_B "Enable experimental work carrier Model B prototype surface" OFF)
set(CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER "AUTO" CACHE STRING
    "Argon2 backend for password hashing: AUTO follows the selected feature preset; SYSTEM requires libargon2; RUNTIME uses dlopen only; OFF disables Argon2")
set_property(CACHE CONFLUX_PASSWORD_HASH_ARGON2_PROVIDER PROPERTY STRINGS AUTO SYSTEM RUNTIME OFF)
set(CONFLUX_JSON_HASH_PROVIDER "AUTO" CACHE STRING
    "JSON object-name hash provider: AUTO follows the selected feature preset; XXHASH requires libxxhash; INTERNAL avoids external hash deps")
set_property(CACHE CONFLUX_JSON_HASH_PROVIDER PROPERTY STRINGS AUTO XXHASH INTERNAL)
set(CONFLUX_GZIP_PROVIDER "AUTO" CACHE STRING
    "Gzip provider: AUTO follows the selected feature preset; ALL enables every discovered backend; LIBDEFLATE, ZLIB_NG, ZLIB, or ISAL require one backend; OFF disables gzip")
set_property(CACHE CONFLUX_GZIP_PROVIDER PROPERTY STRINGS AUTO ALL LIBDEFLATE ZLIB_NG ZLIB ISAL OFF)
set(CONFLUX_BROTLI_PROVIDER "AUTO" CACHE STRING
    "Brotli provider: AUTO follows the selected feature preset; SYSTEM requires it; OFF disables it")
set_property(CACHE CONFLUX_BROTLI_PROVIDER PROPERTY STRINGS AUTO SYSTEM OFF)
set(CONFLUX_ZSTD_PROVIDER "AUTO" CACHE STRING
    "Zstd provider: AUTO follows the selected feature preset; SYSTEM requires it; OFF disables it")
set_property(CACHE CONFLUX_ZSTD_PROVIDER PROPERTY STRINGS AUTO SYSTEM OFF)
set(CONFLUX_TLS_PROVIDER "AUTO" CACHE STRING
    "TLS provider: AUTO follows the selected feature preset; OPENSSL requires it; OFF disables TLS")
set_property(CACHE CONFLUX_TLS_PROVIDER PROPERTY STRINGS AUTO OPENSSL OFF)
set(CONFLUX_HTTP2_PROVIDER "AUTO" CACHE STRING
    "HTTP/2 provider: AUTO follows the selected feature preset; NGHTTP2 requires it; OFF disables HTTP/2")
set_property(CACHE CONFLUX_HTTP2_PROVIDER PROPERTY STRINGS AUTO NGHTTP2 OFF)
set(CONFLUX_HTTP3_PROVIDER "AUTO" CACHE STRING
    "HTTP/3 provider: AUTO follows the selected feature preset and still requires CONFLUX_ENABLE_EXPERIMENTAL=ON; NGTCP2_NGHTTP3_OPENSSL requires it; OFF disables HTTP/3")
set_property(CACHE CONFLUX_HTTP3_PROVIDER PROPERTY STRINGS AUTO NGTCP2_NGHTTP3_OPENSSL OFF)
set(CONFLUX_POSTGRES_PROVIDER "AUTO" CACHE STRING
    "PostgreSQL provider: AUTO follows the selected feature preset; LIBPQ requires it; OFF disables PostgreSQL")
set_property(CACHE CONFLUX_POSTGRES_PROVIDER PROPERTY STRINGS AUTO LIBPQ OFF)
set(CONFLUX_RESOLVED_JSON_HASH_PROVIDER "INTERNAL" CACHE STRING "Resolved JSON object-name hash provider")
set_property(CACHE CONFLUX_RESOLVED_JSON_HASH_PROVIDER PROPERTY STRINGS XXHASH INTERNAL)
set(CONFLUX_RESOLVED_GZIP_PROVIDER "OFF" CACHE STRING
    "Resolved gzip backend for this build; AUTO may populate this from a cached configure-time benchmark")
set_property(CACHE CONFLUX_RESOLVED_GZIP_PROVIDER PROPERTY STRINGS OFF ALL LIBDEFLATE ZLIB_NG ZLIB ISAL)
set(CONFLUX_RESOLVED_BROTLI_PROVIDER "OFF" CACHE STRING "Resolved Brotli provider")
set_property(CACHE CONFLUX_RESOLVED_BROTLI_PROVIDER PROPERTY STRINGS OFF SYSTEM)
set(CONFLUX_RESOLVED_ZSTD_PROVIDER "OFF" CACHE STRING "Resolved zstd provider")
set_property(CACHE CONFLUX_RESOLVED_ZSTD_PROVIDER PROPERTY STRINGS OFF SYSTEM)
set(CONFLUX_RESOLVED_TLS_PROVIDER "OFF" CACHE STRING "Resolved TLS provider")
set_property(CACHE CONFLUX_RESOLVED_TLS_PROVIDER PROPERTY STRINGS OFF OPENSSL)
set(CONFLUX_RESOLVED_HTTP2_PROVIDER "OFF" CACHE STRING "Resolved HTTP/2 provider")
set_property(CACHE CONFLUX_RESOLVED_HTTP2_PROVIDER PROPERTY STRINGS OFF NGHTTP2)
set(CONFLUX_RESOLVED_HTTP3_PROVIDER "OFF" CACHE STRING "Resolved HTTP/3 provider")
set_property(CACHE CONFLUX_RESOLVED_HTTP3_PROVIDER PROPERTY STRINGS OFF NGTCP2_NGHTTP3_OPENSSL)
set(CONFLUX_RESOLVED_POSTGRES_PROVIDER "OFF" CACHE STRING "Resolved PostgreSQL provider")
set_property(CACHE CONFLUX_RESOLVED_POSTGRES_PROVIDER PROPERTY STRINGS OFF LIBPQ)
set(CONFLUX_RESOLVED_ARGON2_PROVIDER "OFF" CACHE STRING "Resolved Argon2 password-hashing provider")
set_property(CACHE CONFLUX_RESOLVED_ARGON2_PROVIDER PROPERTY STRINGS OFF SYSTEM RUNTIME)
option(CONFLUX_WORK_CORO_FRAME_POOL "Enable pooled coroutine frame allocation for work Task/EagerChain frames" OFF)
option(CONFLUX_WORK_ALLOC_STATS "Enable relaxed allocation counters for work.root task/control-block diagnostics" OFF)
option(CONFLUX_WORK_QUEUE_STATS "Enable relaxed WorkPool queue/wake contention counters for profiling" OFF)
option(CONFLUX_GCC_EAGER_MODULE_IMPORTS "Force GCC module imports to eager-load with -fno-module-lazy for local compiler diagnostics" OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        AND CONFLUX_GCC_EAGER_MODULE_IMPORTS
        AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15")
    if(NOT CMAKE_CXX_FLAGS MATCHES "(^| )-fno-module-lazy( |$)")
        string(APPEND CMAKE_CXX_FLAGS " -fno-module-lazy")
    endif()
endif()
option(CONFLUX_GCC_SUGGEST_ATTRIBUTES "Enable noisy GCC -Wsuggest-attribute=pure/const optimization hints" OFF)
set(CONFLUX_SIMD_SELECTION "AUTO" CACHE STRING
    "Select SIMD binding policy for ISA-specific fast paths (AUTO|DIRECT|RUNTIME)")
set_property(CACHE CONFLUX_SIMD_SELECTION PROPERTY STRINGS AUTO DIRECT RUNTIME)
set(CONFLUX_USE_STDSIMD "AUTO" CACHE STRING
    "Use standard/experimental SIMD for library scan paths (AUTO|STD26|STDX|ON|OFF)")
set_property(CACHE CONFLUX_USE_STDSIMD PROPERTY STRINGS AUTO STD26 STDX ON OFF)

if(NOT CONFLUX_SIMD_SELECTION MATCHES "^(AUTO|DIRECT|RUNTIME)$")
    message(FATAL_ERROR
        "conflux: CONFLUX_SIMD_SELECTION must be AUTO, DIRECT, or RUNTIME "
        "(got '${CONFLUX_SIMD_SELECTION}')")
endif()

set(_conflux_simd_selection "${CONFLUX_SIMD_SELECTION}")
if(_conflux_simd_selection STREQUAL "AUTO")
    set(_conflux_simd_selection DIRECT)
endif()
set(_conflux_simd_selection_direct OFF)
set(_conflux_simd_selection_runtime OFF)
if(_conflux_simd_selection STREQUAL "DIRECT")
    set(_conflux_simd_selection_direct ON)
elseif(_conflux_simd_selection STREQUAL "RUNTIME")
    set(_conflux_simd_selection_runtime ON)
else()
    message(FATAL_ERROR "conflux: internal SIMD selection resolution failed")
endif()

if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE"
        AND CONFLUX_HEADER_INTERFACE_WITH_SOURCES
        AND _conflux_simd_selection_runtime)
    message(FATAL_ERROR
        "conflux: CONFLUX_SIMD_SELECTION=RUNTIME is not supported with "
        "HEADER_INTERFACE_WITH_SOURCES yet; use DIRECT or MODULE_INTERFACE")
endif()

set(_conflux_cpu_feature_probes_runtime OFF)
if(_conflux_simd_selection_runtime)
    set(_conflux_cpu_feature_probes_runtime ON)
endif()

set(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF OFF)
set(CONFLUX_EXPERIMENTAL_SEND_ZC OFF)
set(CONFLUX_EXPERIMENTAL_RING_GROWTH OFF)
set(CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST OFF)
if(CONFLUX_ENABLE_EXPERIMENTAL)
    set(CONFLUX_EXPERIMENTAL_RECV_INCREMENTAL_BUF ${CONFLUX_ENABLE_RECV_INCREMENTAL_BUF})
    set(CONFLUX_EXPERIMENTAL_SEND_ZC ${CONFLUX_ENABLE_SEND_ZC})
    set(CONFLUX_EXPERIMENTAL_RING_GROWTH ${CONFLUX_ENABLE_RING_GROWTH})
    set(CONFLUX_EXPERIMENTAL_IOPOLL_STORAGE_TEST ${CONFLUX_ENABLE_IOPOLL_STORAGE_TEST})
endif()

if(CONFLUX_BUILD_BENCHMARKS
    AND (CONFLUX_ENABLE_ASAN OR CONFLUX_ENABLE_UBSAN OR CONFLUX_ENABLE_TSAN)
    AND NOT CONFLUX_ALLOW_SANITIZED_BENCHMARKS)
    message(FATAL_ERROR
        "CONFLUX_BUILD_BENCHMARKS must stay OFF for sanitizer presets. "
        "Use perf-* presets for performance lanes, or set "
        "CONFLUX_ALLOW_SANITIZED_BENCHMARKS=ON for a local instrumented benchmark debug build.")
endif()

if(CONFLUX_PGO_GENERATE AND CONFLUX_PGO_PROFILE_DIR STREQUAL "")
    message(FATAL_ERROR
        "CONFLUX_PGO_GENERATE requires CONFLUX_PGO_PROFILE_DIR. "
        "Use a file pattern for Clang, e.g. /tmp/conflux-pgo/clang/%m-%p.profraw, "
        "or a directory for GCC, e.g. /tmp/conflux-pgo/gcc16.")
endif()

if((CONFLUX_PGO_GENERATE OR NOT CONFLUX_PGO_PROFILE_DIR STREQUAL "")
    AND (CONFLUX_ENABLE_ASAN OR CONFLUX_ENABLE_UBSAN OR CONFLUX_ENABLE_TSAN))
    message(FATAL_ERROR
        "PGO presets must not enable ASan/UBSan/TSan. Generate profiles from "
        "optimized, unsanitized builds and keep sanitizer coverage in the correctness lane.")
endif()
