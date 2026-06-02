function(conflux_enable_import_std_experiment)
    set(_conflux_warn_unsupported FALSE)
    foreach(_conflux_arg IN LISTS ARGN)
        if(_conflux_arg STREQUAL "WARN_UNSUPPORTED")
            set(_conflux_warn_unsupported TRUE)
        else()
            message(FATAL_ERROR "unknown conflux_enable_import_std_experiment argument: ${_conflux_arg}")
        endif()
    endforeach()

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3.0")
        # Covers 4.3.0 up to future stable releases.
        set(_conflux_import_std_uuid "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3")
        # Covers 4.0.3 up to 4.2.x.
        set(_conflux_import_std_uuid "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.0")
        # Covers 4.0.0, 4.0.1, and 4.0.2.
        set(_conflux_import_std_uuid "a9e1cf81-9932-4810-974b-6eccaf14e457")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.8")
        # Covers late 3.31 patches backported with 4.1 features.
        set(_conflux_import_std_uuid "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30.0")
        # Covers initial experimental introduction up to 3.31.7.
        set(_conflux_import_std_uuid "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    endif()

    if(DEFINED _conflux_import_std_uuid)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "${_conflux_import_std_uuid}" PARENT_SCOPE)
    elseif(_conflux_warn_unsupported)
        message(WARNING "Your CMake version does not support experimental 'import std'.")
    endif()
endfunction()
