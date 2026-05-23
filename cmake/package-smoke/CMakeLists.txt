cmake_minimum_required(VERSION 3.30)

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

project(conflux_package_smoke LANGUAGES CXX)

set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated conflux components to require from the install tree")
set(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE "" CACHE STRING
    "Expected installed interface mode, or empty to accept the package mode")
set(CONFLUX_PACKAGE_SMOKE_ENABLE_DB OFF CACHE BOOL
    "Enable DB component smoke lanes when the installed package exposes DB targets")
set(CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS "" CACHE STRING
    "Semicolon-separated conflux components that must not be visible")
set(CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS "" CACHE STRING
    "Semicolon-separated external dependency tokens that must not be resolved")
option(CONFLUX_PACKAGE_SMOKE_FAST_COMPILE
    "Compile package smoke targets without optimization; this validates package wiring, not runtime performance" ON)
set_property(CACHE CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE PROPERTY STRINGS
    "" MODULE_INTERFACE HEADER_INTERFACE)

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

if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    set(CMAKE_CXX_MODULE_STD ${CONFLUX_USE_IMPORT_STD})
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)
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
    "use_import_std=${CONFLUX_USE_IMPORT_STD}\n"
    "package_mock_liburing=${CONFLUX_PACKAGE_MOCK_LIBURING}\n"
    "runtime_requires_liburing=${CONFLUX_RUNTIME_REQUIRES_LIBURING}\n"
    "runtime_mock=${CONFLUX_RUNTIME_MOCK}\n"
    "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "cmake=${CMAKE_VERSION}\n"
    "enable_db=${CONFLUX_PACKAGE_SMOKE_ENABLE_DB}\n"
    "forbidden_components=${CONFLUX_PACKAGE_SMOKE_FORBIDDEN_COMPONENTS}\n"
    "forbidden_external_deps=${CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS}\n"
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
    file(WRITE "${_conflux_package_smoke_source}" [[
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
    file(WRITE "${_conflux_package_smoke_source}" [[
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
    file(WRITE "${_conflux_package_smoke_source}" [[
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
            conflux_add_component_smoke(core core [[
import conflux.types;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(core core [[
#include <conflux/config.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "json")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(json json [[
import conflux.json;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(json json [[
#include <conflux/json.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "http")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(http http [[
import conflux.net.http;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(http http [[
#include <conflux/http.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "file_io_sync")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(file_io_sync file_io_sync [[
import conflux.file_io_sync;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(file_io_sync file_io_sync [[
#include <conflux/file_io_sync.hpp>
int main() { return 0; }
]])
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
    elseif(_component STREQUAL "runtime")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(runtime runtime [[
import conflux.work;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(runtime runtime [[
#include <conflux/runtime.hpp>
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
    set(_conflux_install_prefix "${PACKAGE_PREFIX_DIR}")
    if(NOT _conflux_install_prefix)
        get_filename_component(_conflux_install_prefix "${conflux_DIR}/../../.." ABSOLUTE)
    endif()
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
