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
set_property(CACHE CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE PROPERTY STRINGS
    "" MODULE_INTERFACE HEADER_INTERFACE)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_MODULE_STD OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

enable_testing()

set(_conflux_smoke_components ${CONFLUX_PACKAGE_SMOKE_COMPONENTS})
list(REMOVE_DUPLICATES _conflux_smoke_components)

set(_conflux_find_components ${_conflux_smoke_components})
if("http" IN_LIST _conflux_find_components)
    list(APPEND _conflux_find_components core json runtime)
endif()
if(CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
    list(APPEND _conflux_find_components db)
endif()
list(REMOVE_DUPLICATES _conflux_find_components)

find_package(conflux REQUIRED COMPONENTS ${_conflux_find_components})

if(CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE
        AND NOT CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE STREQUAL CONFLUX_INTERFACE_MODE)
    message(FATAL_ERROR
        "package smoke expected ${CONFLUX_PACKAGE_SMOKE_INTERFACE_MODE}, got ${CONFLUX_INTERFACE_MODE}")
endif()

if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
    set(CMAKE_CXX_MODULE_STD ON)
endif()

set(_conflux_smoke_link_targets conflux::core)
if(TARGET conflux::types)
    list(APPEND _conflux_smoke_link_targets conflux::types)
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

    list(APPEND _conflux_smoke_link_targets ${_target})
endforeach()
list(REMOVE_DUPLICATES _conflux_smoke_link_targets)

set(_conflux_smoke_summary "${CMAKE_CURRENT_BINARY_DIR}/conflux-package-smoke-summary.txt")
file(WRITE "${_conflux_smoke_summary}"
    "components=${_conflux_find_components}\n"
    "requested_components=${_conflux_smoke_components}\n"
    "interface_mode=${CONFLUX_INTERFACE_MODE}\n"
    "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "cmake=${CMAKE_VERSION}\n"
    "enable_db=${CONFLUX_PACKAGE_SMOKE_ENABLE_DB}\n"
)

set(_conflux_package_smoke_source "${CMAKE_CURRENT_BINARY_DIR}/package_smoke.cxx")
if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
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
elseif(CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE")
    file(WRITE "${_conflux_package_smoke_source}" [[
#include <conflux/types.hxx>

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
target_link_libraries(conflux_package_smoke PRIVATE ${_conflux_smoke_link_targets})

add_test(NAME package-smoke/run COMMAND conflux_package_smoke)

function(conflux_add_component_smoke name component source)
    if(NOT TARGET "conflux::${component}")
        message(FATAL_ERROR "package smoke expected target conflux::${component}")
    endif()
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/package_smoke_${name}.cxx")
    file(WRITE "${_source}" "${source}")
    add_executable("conflux_package_smoke_${name}" "${_source}")
    target_compile_features("conflux_package_smoke_${name}" PRIVATE cxx_std_23)
    target_link_libraries("conflux_package_smoke_${name}" PRIVATE "conflux::${component}")
    add_test(NAME "package-smoke/${name}" COMMAND "conflux_package_smoke_${name}")
endfunction()

foreach(_component IN LISTS _conflux_smoke_components)
    if(_component STREQUAL "core")
        conflux_add_component_smoke(core core [[
#include <conflux/config.hpp>
int main() { return 0; }
]])
    elseif(_component STREQUAL "json")
        conflux_add_component_smoke(json json [[
#include <conflux/json.hpp>
int main() { return 0; }
]])
    elseif(_component STREQUAL "http")
        conflux_add_component_smoke(http http [[
#include <conflux/http.hpp>
int main() { return 0; }
]])
    elseif(_component STREQUAL "file_io_sync")
        conflux_add_component_smoke(file_io_sync file_io_sync [[
#include <conflux/file_io_sync.hpp>
int main() { return 0; }
]])
    elseif(_component STREQUAL "runtime")
        conflux_add_component_smoke(runtime runtime [[
#include <conflux/runtime.hpp>
int main() { return 0; }
]])
    elseif(_component STREQUAL "db")
        if(CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
            conflux_add_component_smoke(db db [[
#include <conflux/db.hpp>
int main() { return 0; }
]])
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
