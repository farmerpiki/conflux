if(CONFLUX_BUILD_TESTS)
    add_test(NAME build/no-std-streams
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_no_std_streams.py")
    set_tests_properties(build/no-std-streams PROPERTIES
        LABELS "build;lint"
    )

    add_test(NAME build/cmake-source-files
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-cmake-source-files.py")
    set_tests_properties(build/cmake-source-files PROPERTIES
        LABELS "build;lint"
    )

    add_test(NAME build/component-map
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-component-map.py")
    set_tests_properties(build/component-map PROPERTIES
        LABELS "build;docs;package"
    )

    add_test(NAME build/http-facade-snapshot
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-http-facade-snapshot.py")
    set_tests_properties(build/http-facade-snapshot PROPERTIES
        LABELS "build;http;lint"
    )

    add_test(NAME build/api-surface-map
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-api-surface-map.py")
    set_tests_properties(build/api-surface-map PROPERTIES
        LABELS "build;docs;public-api"
    )

    add_test(NAME build/global-module-exports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-global-module-exports.py")
    set_tests_properties(build/global-module-exports PROPERTIES
        LABELS "build;public-api;lint"
    )

    add_test(NAME build/package-config
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-package-config.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/package-config PROPERTIES
        LABELS "build;package"
    )

    add_test(NAME build/module-fragility-regression
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-module-interface-regressions.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/module-fragility-regression PROPERTIES
        LABELS "build;modules"
        RUN_SERIAL TRUE
    )

    add_test(NAME build/optimized-presets
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-optimized-presets.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/optimized-presets PROPERTIES
        LABELS "build;presets"
        RUN_SERIAL TRUE
    )

    add_test(NAME build/cmake-preset-build-dir
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-cmake-preset-build-dir.py")
    set_tests_properties(build/cmake-preset-build-dir PROPERTIES
        LABELS "build;presets"
    )

    add_test(NAME build/header-first-contact-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-header-first-contact-smoke.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/header-first-contact-smoke PROPERTIES
        LABELS "build;headers;package"
        RUN_SERIAL TRUE
        TIMEOUT 180
    )

    if(CONFLUX_RUN_HEADER_COMPONENT_SMOKE)
        add_test(NAME build/header-component-smoke
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-header-component-smoke.sh"
                    "${CMAKE_SOURCE_DIR}")
        set_tests_properties(build/header-component-smoke PROPERTIES
            LABELS "build;headers;package;expensive"
            RUN_SERIAL TRUE
            TIMEOUT 600
        )
    endif()

    add_test(NAME build/mixed-module-header-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-package-smoke-mixed-module-header.sh")
    set_tests_properties(build/mixed-module-header-smoke PROPERTIES
        LABELS "build;modules;headers;package"
        RUN_SERIAL TRUE
        TIMEOUT 1200
    )

    add_test(NAME build/public-module-import-smoke
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/check-public-module-import-smoke.sh"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(build/public-module-import-smoke PROPERTIES
        LABELS "build;modules;package"
        RUN_SERIAL TRUE
        TIMEOUT 1200
    )

    if(TARGET conflux_simd_direct_shape)
        add_test(NAME build/simd-direct-shape
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --target conflux_simd_direct_shape --config "$<CONFIG>")
        set_tests_properties(build/simd-direct-shape PROPERTIES
            LABELS "build;simd"
            RUN_SERIAL TRUE
        )
    endif()

    add_test(NAME docs/planning-state
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-planning-state.py")
    set_tests_properties(docs/planning-state PROPERTIES
        LABELS "docs;lint"
    )

    add_test(NAME docs/release-docs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-release-docs.py")
    set_tests_properties(docs/release-docs PROPERTIES
        LABELS "docs;lint"
    )

    add_test(NAME docs/first-contact-public-dialect
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-first-contact-public-dialect.py")
    set_tests_properties(docs/first-contact-public-dialect PROPERTIES
        LABELS "docs;lint;public-api"
    )

    add_test(NAME docs/package-docs
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-package-docs.py")
    set_tests_properties(docs/package-docs PROPERTIES
        LABELS "docs;package"
    )

    add_test(NAME docs/release-notes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check-release-notes.py")
    set_tests_properties(docs/release-notes PROPERTIES
        LABELS "docs;release"
    )
endif()
