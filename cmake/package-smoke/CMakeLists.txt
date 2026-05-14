cmake_minimum_required(VERSION 3.30)
project(conflux_package_smoke LANGUAGES CXX)

set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated conflux components to require from the install tree")

find_package(conflux REQUIRED COMPONENTS ${CONFLUX_PACKAGE_SMOKE_COMPONENTS})

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
endforeach()

add_library(conflux_package_smoke INTERFACE)
target_link_libraries(conflux_package_smoke INTERFACE conflux::core)
