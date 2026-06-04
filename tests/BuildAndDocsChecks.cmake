if(CONFLUX_BUILD_TESTS)
    function(conflux_add_python_check test_name script_path labels)
        add_test(NAME ${test_name}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/${script_path}")
        set_tests_properties(${test_name} PROPERTIES
            LABELS "${labels}"
        )
    endfunction()

    conflux_add_python_check(build/no-std-streams
        scripts/check_no_std_streams.py
        "build;lint")
    conflux_add_python_check(build/cmake-source-files
        scripts/check-cmake-source-files.py
        "build;lint")
    conflux_add_python_check(build/component-map
        scripts/check-component-map.py
        "build;docs;package")
    conflux_add_python_check(build/http-facade-snapshot
        scripts/check-http-facade-snapshot.py
        "build;http;lint")
    conflux_add_python_check(build/api-surface-map
        scripts/check-api-surface-map.py
        "build;docs;public-api")
    conflux_add_python_check(build/global-module-exports
        scripts/check-global-module-exports.py
        "build;public-api;lint")

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

    conflux_add_python_check(build/cmake-preset-build-dir
        scripts/check-cmake-preset-build-dir.py
        "build;presets")

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

    conflux_add_python_check(docs/planning-state
        scripts/check-planning-state.py
        "docs;lint")
    conflux_add_python_check(docs/release-docs
        scripts/check-release-docs.py
        "docs;lint")
    conflux_add_python_check(docs/release-skus
        scripts/check-release-skus.py
        "docs;release;package")
    conflux_add_python_check(docs/release-sku-examples
        scripts/check-release-sku-examples.py
        "docs;release;examples")
    conflux_add_python_check(docs/first-contact-public-dialect
        scripts/check-first-contact-public-dialect.py
        "docs;lint;public-api")
    conflux_add_python_check(docs/package-docs
        scripts/check-package-docs.py
        "docs;package")
    conflux_add_python_check(docs/lifecycle
        scripts/check-lifecycle-docs.py
        "docs;http;runtime")
    conflux_add_python_check(docs/cancellation
        scripts/check-cancellation-docs.py
        "docs;runtime;http")
    conflux_add_python_check(docs/security-posture
        scripts/check-security-posture-docs.py
        "docs;http;security")
    conflux_add_python_check(docs/route-openapi
        scripts/check-route-openapi-docs.py
        "docs;http;openapi")
    conflux_add_python_check(docs/release-notes
        scripts/check-release-notes.py
        "docs;release")
endif()
