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

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

enable_testing()

set(CONFLUX_PACKAGE_SMOKE_COMPONENTS "core" CACHE STRING
    "Semicolon-separated conflux components to require from the install tree")

find_package(conflux REQUIRED COMPONENTS ${CONFLUX_PACKAGE_SMOKE_COMPONENTS})

set(_conflux_smoke_link_targets conflux::core)
if(TARGET conflux::types)
    list(APPEND _conflux_smoke_link_targets conflux::types)
endif()
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
target_compile_features(conflux_package_smoke PRIVATE cxx_std_23)
target_link_libraries(conflux_package_smoke PRIVATE ${_conflux_smoke_link_targets})

add_test(NAME package-smoke/run COMMAND conflux_package_smoke)
