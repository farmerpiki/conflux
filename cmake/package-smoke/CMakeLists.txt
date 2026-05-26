cmake_minimum_required(VERSION 3.30)

option(CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD
    "Enable CMake's experimental import-std support for package smoke projects that consume import-std install trees" OFF)
if(CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.8")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30.0")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    endif()
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

set(_conflux_find_components ${_conflux_smoke_components})
list(REMOVE_DUPLICATES _conflux_find_components)

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

set(_conflux_package_smoke_linked_apis ${CONFLUX_PACKAGE_SMOKE_EXERCISE_LINKED_APIS})
if(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE" AND NOT TARGET conflux::header_impl)
	set(_conflux_package_smoke_linked_apis OFF)
endif()

foreach(_component IN LISTS _conflux_find_components)
    if(_component STREQUAL "conflux")
        set(_target conflux::conflux)
    else()
        set(_target "conflux::${_component}")
    endif()

    if(NOT TARGET ${_target})
        message(FATAL_ERROR
            "conflux package smoke expected target '${_target}' for component '${_component}'")
    endif()
endforeach()

foreach(_component IN LISTS conflux_AVAILABLE_COMPONENTS)
    if(_component STREQUAL "conflux")
        set(_target conflux::conflux)
    else()
        set(_target "conflux::${_component}")
    endif()

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
    if(_component STREQUAL "conflux")
        set(_target conflux::conflux)
    else()
        set(_target "conflux::${_component}")
    endif()
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

    if(CONFLUX_PACKAGE_MOCK_LIBURING)
        foreach(_mock_forbidden_header IN ITEMS
                conflux.hxx
                conflux.hpp
                curated.hxx
                curated.hpp
                extended.hxx
                extended.hpp
                complete.hxx
                complete.hpp
                json/reflect.hxx
                json/reflect_provider.hxx)
            if(EXISTS "${_conflux_install_prefix}/include/conflux/${_mock_forbidden_header}")
                message(FATAL_ERROR
                    "mock-liburing package smoke found installed unsupported header: "
                    "${_conflux_install_prefix}/include/conflux/${_mock_forbidden_header}")
            endif()
        endforeach()
    endif()
endif()

set(_conflux_smoke_summary "${CMAKE_CURRENT_BINARY_DIR}/conflux-package-smoke-summary.txt")
file(WRITE "${_conflux_smoke_summary}"
    "components=${_conflux_find_components}\n"
    "requested_components=${_conflux_smoke_components}\n"
    "available_components=${conflux_AVAILABLE_COMPONENTS}\n"
    "available_targets=${conflux_AVAILABLE_TARGETS}\n"
    "available_support_targets=${conflux_AVAILABLE_SUPPORT_TARGETS}\n"
    "visible_components=${conflux_VISIBLE_COMPONENTS}\n"
    "visible_targets=${conflux_VISIBLE_TARGETS}\n"
    "visible_support_targets=${conflux_VISIBLE_SUPPORT_TARGETS}\n"
    "resolved_external_deps=${conflux_RESOLVED_EXTERNAL_DEPS}\n"
    "interface_mode=${CONFLUX_INTERFACE_MODE}\n"
    "expected_api_surface=${CONFLUX_PACKAGE_SMOKE_API_SURFACE}\n"
    "use_import_std=${CONFLUX_USE_IMPORT_STD}\n"
    "package_mock_liburing=${CONFLUX_PACKAGE_MOCK_LIBURING}\n"
    "runtime_requires_liburing=${CONFLUX_RUNTIME_REQUIRES_LIBURING}\n"
    "runtime_mock=${CONFLUX_RUNTIME_MOCK}\n"
    "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "cmake=${CMAKE_VERSION}\n"
	"enable_db=${CONFLUX_PACKAGE_SMOKE_ENABLE_DB}\n"
	"forbidden_components=${CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS}\n"
	"forbidden_external_deps=${CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS}\n"
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

set(_conflux_package_smoke_source "${CMAKE_CURRENT_BINARY_DIR}/package_smoke.cxx")
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE" AND CONFLUX_USE_IMPORT_STD)
    file(WRITE "${_conflux_package_smoke_source}" "${_conflux_package_smoke_api_surface_check}" [[
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
    file(WRITE "${_conflux_package_smoke_source}" "${_conflux_package_smoke_api_surface_check}" [[
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
    file(WRITE "${_conflux_package_smoke_source}" "${_conflux_package_smoke_api_surface_check}" [[
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
if(TARGET conflux::headers)
    target_link_libraries(conflux_package_smoke PRIVATE conflux::headers)
endif()
if(TARGET conflux::core)
    target_link_libraries(conflux_package_smoke PRIVATE conflux::core)
endif()
if(TARGET conflux::types)
    target_link_libraries(conflux_package_smoke PRIVATE conflux::types)
endif()
foreach(_component IN LISTS _conflux_find_components)
    if(TARGET "conflux::${_component}")
        target_link_libraries(conflux_package_smoke PRIVATE "conflux::${_component}")
    endif()
endforeach()

add_test(NAME package-smoke/run COMMAND conflux_package_smoke)

function(conflux_package_append_target_closure out target)
    if(NOT TARGET ${target})
        return()
    endif()
    set(_seen ${${out}})
    if("${target}" IN_LIST _seen)
        return()
    endif()
    list(APPEND _seen ${target})
    foreach(_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_libs ${target} ${_prop})
        if(NOT _libs)
            continue()
        endif()
        foreach(_lib IN LISTS _libs)
            if(_lib MATCHES "conflux::[A-Za-z0-9_]+")
                string(REGEX MATCH "conflux::[A-Za-z0-9_]+" _dep "${_lib}")
                if(TARGET ${_dep})
                    set(${out} "${_seen}" PARENT_SCOPE)
                    conflux_package_append_target_closure(${out} ${_dep})
                    set(_seen ${${out}})
                endif()
            endif()
        endforeach()
    endforeach()
    set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

function(conflux_package_collect_module_smoke_sources out)
    set(_sources)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_sets ${_target} CXX_MODULE_SETS)
        if(_sets)
            foreach(_set IN LISTS _sets)
                get_target_property(_set_sources ${_target} CXX_MODULE_SET_${_set})
                if(_set_sources)
                    list(APPEND _sources ${_set_sources})
                endif()
            endforeach()
        endif()
        get_target_property(_interface_sets ${_target} INTERFACE_CXX_MODULE_SETS)
        if(_interface_sets)
            foreach(_set IN LISTS _interface_sets)
                get_target_property(_set_sources ${_target} INTERFACE_CXX_MODULE_SET_${_set})
                if(_set_sources)
                    list(APPEND _sources ${_set_sources})
                endif()
            endforeach()
        endif()
    endforeach()
    if(_sources)
        list(REMOVE_DUPLICATES _sources)
    endif()
    set(${out} "${_sources}" PARENT_SCOPE)
endfunction()

if(CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS
        AND NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    message(FATAL_ERROR
        "CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS requires a MODULE_INTERFACE install tree")
endif()

if(CONFLUX_PACKAGE_SMOKE_PUBLIC_MODULE_IMPORTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_conflux_module_smoke_targets)
    foreach(_component IN LISTS _conflux_find_components)
        if(_component STREQUAL "conflux")
            set(_target conflux::conflux)
        else()
            set(_target "conflux::${_component}")
        endif()
        if(TARGET ${_target})
            conflux_package_append_target_closure(
                _conflux_module_smoke_targets ${_target})
        endif()
    endforeach()
    if(_conflux_module_smoke_targets)
        list(REMOVE_DUPLICATES _conflux_module_smoke_targets)
    endif()
    conflux_package_collect_module_smoke_sources(
        _conflux_module_smoke_sources ${_conflux_module_smoke_targets})
    if(NOT _conflux_module_smoke_sources)
        message(FATAL_ERROR
            "package smoke public module import matrix found no module sources")
    endif()

    set(_conflux_module_smoke_dir
        "${CMAKE_CURRENT_BINARY_DIR}/public-module-import-smoke")
    set(_conflux_module_smoke_source_list
        "${_conflux_module_smoke_dir}/public-module-sources.txt")
    set(_conflux_module_smoke_fragment
        "${_conflux_module_smoke_dir}/public-module-imports.cmake")
    file(MAKE_DIRECTORY "${_conflux_module_smoke_dir}")
    file(WRITE "${_conflux_module_smoke_source_list}" "")
    foreach(_source IN LISTS _conflux_module_smoke_sources)
        file(APPEND "${_conflux_module_smoke_source_list}" "${_source}\n")
    endforeach()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/../../scripts/generate-public-module-import-smoke.py"
                --source-list "${_conflux_module_smoke_source_list}"
                --out-dir "${_conflux_module_smoke_dir}/sources"
                --cmake-fragment "${_conflux_module_smoke_fragment}"
        RESULT_VARIABLE _conflux_module_smoke_result)
    if(NOT _conflux_module_smoke_result EQUAL 0)
        message(FATAL_ERROR
            "package smoke public module import matrix generation failed")
    endif()
    include("${_conflux_module_smoke_fragment}")
    if(NOT CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES)
        message(FATAL_ERROR
            "package smoke public module import matrix generated no sources")
    endif()

    add_library(conflux_package_smoke_public_module_imports OBJECT
        ${CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES})
    target_compile_features(conflux_package_smoke_public_module_imports PRIVATE cxx_std_23)
    conflux_apply_package_smoke_build_policy(
        conflux_package_smoke_public_module_imports)
    target_link_libraries(conflux_package_smoke_public_module_imports PRIVATE
        ${_conflux_module_smoke_targets})
endif()

if(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER)
    if(NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
        message(FATAL_ERROR
            "CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER requires a MODULE_INTERFACE install tree")
    endif()

    set(_conflux_mixed_import_source "${CMAKE_CURRENT_BINARY_DIR}/mixed_import.cxx")
    set(_conflux_mixed_include_source "${CMAKE_CURRENT_BINARY_DIR}/mixed_include.cxx")
    set(_conflux_mixed_main_source "${CMAKE_CURRENT_BINARY_DIR}/mixed_main.cxx")

    set(_conflux_mixed_expected 42)
    set(_conflux_mixed_import_modules "import conflux.types;
")
    set(_conflux_mixed_import_checks [[
    std::optional<int> value{41};
    int score = value.value_or(0) + 1;
]])
    set(_conflux_mixed_include_headers [[#include <conflux/types.hpp>
]])
    set(_conflux_mixed_include_checks [[
    std::optional<int> value{57};
    int score = value.value_or(0) - 15;
]])

    if(TARGET conflux::json)
        math(EXPR _conflux_mixed_expected "${_conflux_mixed_expected} + 1")
        string(APPEND _conflux_mixed_import_modules "import conflux.json;
")
        string(APPEND _conflux_mixed_import_checks [[
    auto parsed = conflux::json::parse(R"({"x":1})");
    if (!parsed.has_value()) {
        return -1;
    }
    score += 1;
]])
        string(APPEND _conflux_mixed_include_headers "#include <conflux/json.hpp>
")
        string(APPEND _conflux_mixed_include_checks [[
    auto parsed = conflux::json::parse(R"({"x":1})");
    if (!parsed.has_value()) {
        return -1;
    }
    score += 1;
]])
    endif()

    if(TARGET conflux::http)
        math(EXPR _conflux_mixed_expected "${_conflux_mixed_expected} + 1")
        string(APPEND _conflux_mixed_import_modules "import conflux.http;
")
        string(APPEND _conflux_mixed_import_checks [[
    auto response = conflux::http::text("ok");
    if (response.status != 200 || response.text_body() != "ok") {
        return -2;
    }
    score += 1;
]])
        string(APPEND _conflux_mixed_include_headers "#include <conflux/http.hpp>
")
        string(APPEND _conflux_mixed_include_checks [[
    auto response = conflux::http::text("ok");
    if (response.status != 200 || response.text_body() != "ok") {
        return -2;
    }
    score += 1;
]])
    endif()

    if(CONFLUX_USE_IMPORT_STD)
        set(_conflux_mixed_import_std_prelude "import std;
")
    else()
        set(_conflux_mixed_import_std_prelude "#include <optional>
")
    endif()

    file(WRITE "${_conflux_mixed_import_source}" "${_conflux_mixed_import_std_prelude}
${_conflux_mixed_import_modules}
int conflux_mixed_imported() {
${_conflux_mixed_import_checks}    return score;
}
")

    file(WRITE "${_conflux_mixed_include_source}" "${_conflux_mixed_include_headers}
#include <optional>

int conflux_mixed_included() {
${_conflux_mixed_include_checks}    return score;
}
")

    file(WRITE "${_conflux_mixed_main_source}" "int conflux_mixed_imported();
int conflux_mixed_included();

int main() {
    return (conflux_mixed_imported() == ${_conflux_mixed_expected} && conflux_mixed_included() == ${_conflux_mixed_expected}) ? 0 : 1;
}
")

    add_executable(conflux_package_smoke_mixed_module_header
        "${_conflux_mixed_import_source}"
        "${_conflux_mixed_include_source}"
        "${_conflux_mixed_main_source}")
    target_compile_features(conflux_package_smoke_mixed_module_header PRIVATE cxx_std_23)
    conflux_apply_package_smoke_build_policy(conflux_package_smoke_mixed_module_header)
    if(TARGET conflux::headers)
        target_link_libraries(conflux_package_smoke_mixed_module_header PRIVATE conflux::headers)
    endif()
    if(TARGET conflux::core)
        target_link_libraries(conflux_package_smoke_mixed_module_header PRIVATE conflux::core)
    endif()
    if(TARGET conflux::types)
        target_link_libraries(conflux_package_smoke_mixed_module_header PRIVATE conflux::types)
    endif()
    foreach(_component IN LISTS _conflux_find_components)
        if(TARGET "conflux::${_component}")
            target_link_libraries(conflux_package_smoke_mixed_module_header PRIVATE "conflux::${_component}")
        endif()
    endforeach()
    add_test(NAME package-smoke/mixed-module-header COMMAND conflux_package_smoke_mixed_module_header)
endif()

function(conflux_add_component_smoke name component source)
    if(NOT TARGET "conflux::${component}")
        message(FATAL_ERROR "package smoke expected target conflux::${component}")
    endif()
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/package_smoke_${name}.cxx")
    file(WRITE "${_source}" "${source}")
    add_executable("conflux_package_smoke_${name}" "${_source}")
    target_compile_features("conflux_package_smoke_${name}" PRIVATE cxx_std_23)
    conflux_apply_package_smoke_build_policy("conflux_package_smoke_${name}")
    if(TARGET conflux::headers)
        target_link_libraries("conflux_package_smoke_${name}" PRIVATE conflux::headers)
    endif()
    if(TARGET conflux::core)
        target_link_libraries("conflux_package_smoke_${name}" PRIVATE conflux::core)
    endif()
    if(TARGET conflux::types)
        target_link_libraries("conflux_package_smoke_${name}" PRIVATE conflux::types)
    endif()
    target_link_libraries("conflux_package_smoke_${name}" PRIVATE "conflux::${component}")
    add_test(NAME "package-smoke/${name}" COMMAND "conflux_package_smoke_${name}")
endfunction()

foreach(_component IN LISTS _conflux_smoke_components)
	if(_component STREQUAL "core")
		if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(core core [[
import conflux.types;
int main() {
    auto summary = conflux::build_info_summary();
    return summary.empty() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(core core [[
import conflux.types;
int main() { return 0; }
]])
			endif()
		else()
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(core core [[
#include <conflux/config.hpp>
int main() {
    auto summary = conflux::build_info_summary();
    return summary.empty() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(core core [[
#include <conflux/config.hpp>
int main() { return 0; }
]])
			endif()
		endif()
	elseif(_component STREQUAL "json")
		if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(json json [[
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
import conflux.features;
import conflux.json;
static_assert(conflux::HAS_JSON);
int main() {
    auto parsed = conflux::json::parse("{\"x\":1}");
    return parsed.has_value() ? 0 : 1;
}
]])
			else()
				conflux_add_component_smoke(json json [[
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
import conflux.features;
import conflux.json;
static_assert(conflux::HAS_JSON);
int main() { return 0; }
]])
			endif()
		else()
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(json json [[
#include <conflux/features.hxx>
#include <conflux/json.hpp>
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
#if !CONFLUX_SURFACE_HAS_JSON
#error "conflux::json package target must publish CONFLUX_SURFACE_HAS_JSON=1"
#endif
static_assert(conflux::HAS_JSON);
int main() {
    auto parsed = conflux::json::parse("{\"x\":1}");
    return parsed.has_value() ? 0 : 1;
}
]])
			else()
				conflux_add_component_smoke(json json [[
#include <conflux/features.hxx>
#include <conflux/json.hpp>
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
#if !CONFLUX_SURFACE_HAS_JSON
#error "conflux::json package target must publish CONFLUX_SURFACE_HAS_JSON=1"
#endif
static_assert(conflux::HAS_JSON);
int main() { return 0; }
]])
			endif()
		endif()
    elseif(_component STREQUAL "http")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(http http [[
import conflux.http;
int main() {
    auto app = conflux::http::app();
    app.get("/health", [] { return conflux::http::text("ok"); }).name("health.check");
    return app.routes().empty() ? 1 : 0;
}
]])
        else()
            conflux_add_component_smoke(http http [[
#include <conflux/http.hpp>
int main() {
    auto app = conflux::http::app();
    app.get("/health", [] { return conflux::http::text("ok"); }).name("health.check");
    return app.routes().empty() ? 1 : 0;
}
]])
        endif()
	elseif(_component STREQUAL "file_io_sync")
		if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(file_io_sync file_io_sync [[
import conflux.file_io_sync;
int main() {
    auto text = read_text_file_nothrow("/definitely/not/a/conflux/package-smoke-file");
    return text.has_value() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(file_io_sync file_io_sync [[
import conflux.file_io_sync;
int main() { return 0; }
]])
			endif()
		else()
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(file_io_sync file_io_sync [[
#include <conflux/file_io_sync.hpp>
int main() {
    auto text = read_text_file_nothrow("/definitely/not/a/conflux/package-smoke-file");
    return text.has_value() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(file_io_sync file_io_sync [[
#include <conflux/file_io_sync.hpp>
int main() { return 0; }
]])
			endif()
		endif()
    elseif(_component STREQUAL "template")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(template template [[
import conflux.templates;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(template template [[
#include <conflux/template.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "dns")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(dns dns [[
import conflux.net.dns;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(dns dns [[
#include <conflux/dns.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "work")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(work work [[
import conflux.work;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(work work [[
#include <conflux/work.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "db")
        if(CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
            if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
                conflux_add_component_smoke(db db [[
import conflux.db;
int main() { return 0; }
]])
            else()
                conflux_add_component_smoke(db db [[
#include <conflux/db.hpp>
#include <conflux/pg/types.hxx>
int main() { return 0; }
]])
            endif()
        endif()
    elseif(_component STREQUAL "pg")
        if(CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
            if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
                conflux_add_component_smoke(pg pg [[
import conflux.pg;
int main() { return 0; }
]])
            else()
                conflux_add_component_smoke(pg pg [[
#include <conflux/pg/types.hxx>
int main() { return 0; }
]])
            endif()
        endif()
    endif()
endforeach()

if(NOT CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
    set(_db_headers
        "${_conflux_install_prefix}/include/conflux/db.hxx"
        "${_conflux_install_prefix}/include/conflux/db.hpp"
        "${_conflux_install_prefix}/include/conflux/pg.hxx"
        "${_conflux_install_prefix}/include/conflux/pg.hpp")
    foreach(_db_header IN LISTS _db_headers)
        if(EXISTS "${_db_header}")
            message(FATAL_ERROR "DB-disabled package smoke found installed DB header: ${_db_header}")
        endif()
    endforeach()
    if(EXISTS "${_conflux_install_prefix}/include/conflux/pg")
        message(FATAL_ERROR
            "DB-disabled package smoke found installed DB include directory: ${_conflux_install_prefix}/include/conflux/pg")
    endif()
endif()
