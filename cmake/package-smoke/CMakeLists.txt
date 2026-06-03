cmake_minimum_required(VERSION 3.30)

option(CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD
    "Enable CMake's experimental import-std support for package smoke projects that consume import-std install trees" OFF)
if(CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD)
    include("${CMAKE_CURRENT_LIST_DIR}/../ConfluxImportStdGate.cmake")
    conflux_enable_import_std_experiment()
endif()

project(conflux_package_smoke LANGUAGES CXX)

set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated conflux components to require from the install tree")
set(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE "" CACHE STRING
    "Expected installed interface mode, or empty to accept the package mode")
set(CONFLUX_PACKAGE_SMOKE_API_SURFACE "" CACHE STRING
    "Expected installed aggregate API surface, or empty to skip the check")
set(CONFLUX_PACKAGE_SMOKE_ENABLE_DB OFF CACHE BOOL
    "Enable DB component smoke lanes when the installed package exposes DB targets")
set(CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS "" CACHE STRING
    "Semicolon-separated conflux components that must not be visible")
set(CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS "" CACHE STRING
	"Semicolon-separated external dependency tokens that must not be resolved")
option(CONFLUX_PACKAGE_SMOKE_EXERCISE_LINKED_APIS
	"Run tiny downstream API calls that require installed implementation symbols when this package mode supports them"
	ON)
option(CONFLUX_PACKAGE_SMOKE_FAST_COMPILE
	"Compile package smoke targets without optimization; this validates package wiring, not runtime performance" ON)
option(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER
	"Build a downstream consumer with one TU importing conflux modules and another including conflux headers" OFF)
option(CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS
    "Compile one downstream import-only TU for each public module file set exposed by requested package targets" OFF)
set_property(CACHE CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE PROPERTY STRINGS
    "" MODULE_INTERFACE HEADER_INTERFACE)
set_property(CACHE CONFLUX_PACKAGE_SMOKE_API_SURFACE PROPERTY STRINGS
    "" curated extended complete)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_MODULE_STD OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

enable_testing()

set(_conflux_smoke_components ${CONFLUX_PACKAGE_SMOKE_COMPONENTS})
list(REMOVE_DUPLICATES _conflux_smoke_components)
if(NOT _conflux_smoke_components)
    message(FATAL_ERROR
        "CONFLUX_PACKAGE_SMOKE_COMPONENTS must name at least one component")
endif()

set(_conflux_find_components ${_conflux_smoke_components})
list(REMOVE_DUPLICATES _conflux_find_components)
foreach(_component IN LISTS _conflux_find_components)
    if(_component MATCHES "^_" OR _component MATCHES "^header_impl(_|$)" OR _component STREQUAL "headers")
        message(FATAL_ERROR
            "CONFLUX_PACKAGE_SMOKE_COMPONENTS must request public components, not support component '${_component}'")
    endif()
endforeach()

find_package(conflux REQUIRED COMPONENTS ${_conflux_find_components})

if(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE
        AND NOT CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE STREQUAL CONFLUX_INTERFACE_MODE)
    message(FATAL_ERROR
        "package smoke expected ${CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE}, got ${CONFLUX_INTERFACE_MODE}")
endif()
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE"
        AND CONFLUX_USE_IMPORT_STD
        AND NOT CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD)
    message(FATAL_ERROR
        "Installed package uses import std; rerun package smoke with "
        "CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD=ON to opt into CMake's experimental support.")
endif()

if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
	set(CMAKE_CXX_MODULE_STD ${CONFLUX_USE_IMPORT_STD})
	set(CMAKE_CXX_SCAN_FOR_MODULES ON)
    if(CONFLUX_USE_IMPORT_STD)
        conflux_configure_std_module_sources(
            SUPPRESS_CLANG_RESERVED_MODULE_IDENTIFIER_WARNING)
    endif()
endif()

set(_conflux_package_smoke_api_surface_check "")
if(CONFLUX_PACKAGE_SMOKE_API_SURFACE)
	if(NOT CONFLUX_PACKAGE_SMOKE_API_SURFACE MATCHES "^(curated|extended|complete)$")
		message(FATAL_ERROR
			"CONFLUX_PACKAGE_SMOKE_API_SURFACE must be curated, extended, complete, or empty")
	endif()
	string(TOUPPER "${CONFLUX_PACKAGE_SMOKE_API_SURFACE}" _conflux_expected_surface_upper)
	string(APPEND _conflux_package_smoke_api_surface_check
		"#if CONFLUX_API_SURFACE_LEVEL != CONFLUX_API_SURFACE_${_conflux_expected_surface_upper}\n"
		"#error \"installed conflux API surface does not match CONFLUX_PACKAGE_SMOKE_API_SURFACE\"\n"
		"#endif\n")
endif()

function(conflux_package_smoke_surface_macros_for_component out component)
    string(TOUPPER "${component}" _macro)
    set(_macros "${_macro}")
    if(component STREQUAL "http")
        list(APPEND _macros HTTP_FACADE)
    elseif(component STREQUAL "net_config")
        list(APPEND _macros HTTP_CONFIG)
    elseif(component STREQUAL "http_observability")
        list(APPEND _macros HTTP_METRICS)
    elseif(component STREQUAL "http_auth")
        list(APPEND _macros TLS_JWT)
    elseif(component STREQUAL "template")
        list(APPEND _macros TEMPLATES)
    elseif(component STREQUAL "template_watch")
        list(APPEND _macros TEMPLATES_WATCH)
    elseif(component STREQUAL "pg")
        list(APPEND _macros DB DB_COMPAT)
    elseif(component STREQUAL "db")
        set(_macros DB DB_COMPAT)
    endif()
    list(REMOVE_DUPLICATES _macros)
    set(${out} "${_macros}" PARENT_SCOPE)
endfunction()

set(_conflux_package_smoke_forbidden_surface_check "")
set(_conflux_package_smoke_forbidden_surface_macros)
foreach(_component IN LISTS CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS)
    conflux_package_smoke_surface_macros_for_component(_macros "${_component}")
    list(APPEND _conflux_package_smoke_forbidden_surface_macros ${_macros})
endforeach()
list(REMOVE_DUPLICATES _conflux_package_smoke_forbidden_surface_macros)
foreach(_macro IN LISTS _conflux_package_smoke_forbidden_surface_macros)
    string(APPEND _conflux_package_smoke_forbidden_surface_check
        "#if CONFLUX_SURFACE_HAS_${_macro}\n"
        "#error \"installed package exposes forbidden surface macro CONFLUX_SURFACE_HAS_${_macro}\"\n"
        "#endif\n")
endforeach()

set(_conflux_package_smoke_linked_apis ${CONFLUX_PACKAGE_SMOKE_EXERCISE_LINKED_APIS})
if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE" AND NOT TARGET conflux::header_impl)
	set(_conflux_package_smoke_linked_apis OFF)
endif()

foreach(_component IN LISTS _conflux_find_components)
    set(_target "conflux::${_component}")
    if(NOT TARGET ${_target})
        message(FATAL_ERROR
            "conflux package smoke expected target '${_target}' for component '${_component}'")
    endif()
endforeach()

if(umbrella IN_LIST _conflux_find_components AND NOT TARGET conflux::conflux)
    message(FATAL_ERROR
        "conflux package smoke expected canonical aggregate target 'conflux::conflux' for component 'umbrella'")
endif()

foreach(_component IN LISTS conflux_AVAILABLE_COMPONENTS)
    set(_target "conflux::${_component}")
    if(TARGET ${_target} AND NOT _component IN_LIST conflux_VISIBLE_COMPONENTS)
        message(FATAL_ERROR
            "conflux package smoke found unrequested visible target '${_target}'")
    endif()
endforeach()

foreach(_component IN LISTS CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS)
    if(_component IN_LIST conflux_AVAILABLE_COMPONENTS)
        message(FATAL_ERROR
            "conflux package smoke found forbidden available component '${_component}'")
    endif()
    if(_component IN_LIST conflux_VISIBLE_COMPONENTS)
        message(FATAL_ERROR
            "conflux package smoke found forbidden visible component '${_component}'")
    endif()
    set(_target "conflux::${_component}")
    if(TARGET ${_target})
        message(FATAL_ERROR
            "conflux package smoke found forbidden imported target '${_target}'")
    endif()
endforeach()

foreach(_external_dep IN LISTS CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS)
    if(_external_dep IN_LIST conflux_RESOLVED_EXTERNAL_DEPS)
        message(FATAL_ERROR
            "conflux package smoke resolved forbidden external dependency '${_external_dep}'")
    endif()
endforeach()

set(_conflux_install_prefix "${PACKAGE_PREFIX_DIR}")
if(NOT _conflux_install_prefix)
    get_filename_component(_conflux_install_prefix "${conflux_DIR}/../../.." ABSOLUTE)
endif()

if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    foreach(_component IN LISTS CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS)
        foreach(_suffix IN ITEMS hxx hpp)
            set(_forbidden_header
                "${_conflux_install_prefix}/include/conflux/${_component}.${_suffix}")
            if(EXISTS "${_forbidden_header}")
                message(FATAL_ERROR
                    "package smoke found installed forbidden component header: ${_forbidden_header}")
            endif()
        endforeach()
        if(EXISTS "${_conflux_install_prefix}/include/conflux/${_component}")
            message(FATAL_ERROR
                "package smoke found installed forbidden component include directory: "
                "${_conflux_install_prefix}/include/conflux/${_component}")
        endif()
    endforeach()

endif()

set(_conflux_smoke_summary "${CMAKE_CURRENT_BINARY_DIR}/conflux-package-smoke-summary.txt")
file(WRITE "${_conflux_smoke_summary}"
    "components=${_conflux_find_components}\n"
    "requested_components=${_conflux_smoke_components}\n"
    "available_components=${conflux_AVAILABLE_COMPONENTS}\n"
    "available_targets=${conflux_AVAILABLE_TARGETS}\n"
    "available_explicit_targets=${conflux_AVAILABLE_EXPLICIT_TARGETS}\n"
    "available_experimental_targets=${conflux_AVAILABLE_EXPERIMENTAL_TARGETS}\n"
    "available_support_targets=${conflux_AVAILABLE_SUPPORT_TARGETS}\n"
    "visible_components=${conflux_VISIBLE_COMPONENTS}\n"
    "visible_targets=${conflux_VISIBLE_TARGETS}\n"
    "visible_explicit_targets=${conflux_VISIBLE_EXPLICIT_TARGETS}\n"
    "visible_experimental_targets=${conflux_VISIBLE_EXPERIMENTAL_TARGETS}\n"
    "visible_support_targets=${conflux_VISIBLE_SUPPORT_TARGETS}\n"
    "resolved_external_deps=${conflux_RESOLVED_EXTERNAL_DEPS}\n"
    "interface_mode=${CONFLUX_INTERFACE_MODE}\n"
    "expected_api_surface=${CONFLUX_PACKAGE_SMOKE_API_SURFACE}\n"
    "use_import_std=${CONFLUX_USE_IMPORT_STD}\n"
    "runtime_requires_liburing=${CONFLUX_RUNTIME_REQUIRES_LIBURING}\n"
    "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "cmake=${CMAKE_VERSION}\n"
	"enable_db=${CONFLUX_PACKAGE_SMOKE_ENABLE_DB}\n"
	"forbidden_components=${CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS}\n"
	"forbidden_external_deps=${CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS}\n"
	"forbidden_surface_macros=${_conflux_package_smoke_forbidden_surface_macros}\n"
	"exercise_linked_apis=${_conflux_package_smoke_linked_apis}\n"
	"mixed_module_header=${CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER}\n"
    "public_module_imports=${CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS}\n"
)

function(conflux_apply_package_smoke_build_policy target)
    if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
        set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endif()
    if(CONFLUX_PACKAGE_SMOKE_FAST_COMPILE)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-O0>
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Od>)
    endif()
endfunction()

function(conflux_link_package_smoke_base_targets target)
    if(TARGET conflux::headers)
        target_link_libraries(${target} PRIVATE conflux::headers)
    endif()
    if(TARGET conflux::core)
        target_link_libraries(${target} PRIVATE conflux::core)
    endif()
    if(TARGET conflux::types)
        target_link_libraries(${target} PRIVATE conflux::types)
    endif()
endfunction()

set(_conflux_package_smoke_source "${CMAKE_CURRENT_BINARY_DIR}/package_smoke.cxx")
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE" AND CONFLUX_USE_IMPORT_STD)
    file(WRITE "${_conflux_package_smoke_source}"
        "${_conflux_package_smoke_api_surface_check}${_conflux_package_smoke_forbidden_surface_check}" [[
import std;
import conflux.types;

int main() {
    std::string value{"conflux"};
    std::vector<std::string> values{};
    values.emplace_back(value);

    std::optional<std::size_t> count{values.size()};
    if (not count.has_value()) {
        return 1;
    }

    return (*count == std::size_t{1} and values.front() == value) ? 0 : 1;
}
]])
elseif(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    file(WRITE "${_conflux_package_smoke_source}"
        "${_conflux_package_smoke_api_surface_check}${_conflux_package_smoke_forbidden_surface_check}" [[
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

import conflux.types;

int main() {
    std::string value{"conflux"};
    std::vector<std::string> values{};
    values.emplace_back(value);

    std::optional<std::size_t> count{values.size()};
    if (not count.has_value()) {
        return 1;
    }

    return (*count == std::size_t{1} and values.front() == value) ? 0 : 1;
}
]])
elseif(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    file(WRITE "${_conflux_package_smoke_source}"
        "${_conflux_package_smoke_api_surface_check}${_conflux_package_smoke_forbidden_surface_check}" [[
#include <conflux/types.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

int main() {
    std::string value{"conflux"};
    std::vector<std::string> values{};
    values.emplace_back(value);

    std::optional<std::size_t> count{values.size()};
    if (not count.has_value()) {
        return 1;
    }

    return (*count == std::size_t{1} and values.front() == value) ? 0 : 1;
}
]])
else()
    message(FATAL_ERROR
        "package smoke found unknown CONFLUX_INTERFACE_MODE='${CONFLUX_INTERFACE_MODE}'")
endif()

add_executable(conflux_package_smoke "${_conflux_package_smoke_source}")
target_compile_features(conflux_package_smoke PRIVATE cxx_std_23)
conflux_apply_package_smoke_build_policy(conflux_package_smoke)
conflux_link_package_smoke_base_targets(conflux_package_smoke)
foreach(_component IN LISTS _conflux_find_components)
    if(TARGET "conflux::${_component}")
        target_link_libraries(conflux_package_smoke PRIVATE "conflux::${_component}")
    endif()
endforeach()

add_test(NAME package-smoke/run COMMAND conflux_package_smoke)

include("${CMAKE_CURRENT_LIST_DIR}/PublicModuleImports.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/MixedModuleHeader.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ComponentSmokes.cmake")
