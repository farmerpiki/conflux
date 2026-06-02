if(TARGET conflux_core)
    add_executable(conflux_api_surface_core_import_smoke
        api_surface_core_import_smoke.cxx)
    target_link_libraries(conflux_api_surface_core_import_smoke
        PRIVATE conflux_core conflux_options)
    add_test(NAME api-surface/import-core
        COMMAND conflux_api_surface_core_import_smoke)
    set_tests_properties(api-surface/import-core PROPERTIES
        LABELS "build;api-surface;modules"
        RUN_SERIAL TRUE)
endif()

if(TARGET conflux)
    foreach(_surface IN ITEMS curated extended complete selected)
        add_executable(conflux_api_surface_${_surface}_import_smoke
            api_surface_${_surface}_import_smoke.cxx)
        target_link_libraries(conflux_api_surface_${_surface}_import_smoke
            PRIVATE conflux conflux_options)
        add_test(NAME api-surface/import-${_surface}
            COMMAND conflux_api_surface_${_surface}_import_smoke)
        set_tests_properties(api-surface/import-${_surface} PROPERTIES
            LABELS "build;api-surface;modules"
            RUN_SERIAL TRUE)
    endforeach()
endif()
