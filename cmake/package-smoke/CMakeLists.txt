cmake_minimum_required(VERSION 3.30)
project(conflux_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

enable_testing()

set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated conflux components to require from the install tree")

find_package(conflux REQUIRED COMPONENTS ${CONFLUX_PACKAGE_SMOKE_COMPONENTS})

set(_conflux_smoke_link_targets conflux::core)
foreach(_component IN LISTS CONFLUX_PACKAGE_SMOKE_COMPONENTS)
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

add_executable(conflux_package_smoke package_smoke.cxx)
target_compile_features(conflux_package_smoke PRIVATE cxx_std_26)
target_link_libraries(conflux_package_smoke PRIVATE ${_conflux_smoke_link_targets})

add_test(NAME package-smoke/run COMMAND conflux_package_smoke)
