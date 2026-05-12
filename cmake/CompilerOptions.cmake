# cmake/CompilerOptions.cmake
# Applies strict warnings and optional sanitisers to an INTERFACE target.

function(conflux_apply_compiler_options target)
    # ── Common flags ─────────────────────────────────────────────────────────
    target_compile_options(${target} INTERFACE
        -Wall
        -Wcast-align
        -Wconversion
        -Wdisabled-optimization
        -Wdouble-promotion
        -Wextra
        -Wformat=2
        -Wimplicit-fallthrough
        -Wnon-virtual-dtor
        -Wnull-dereference
        -Wold-style-cast
        -Woverloaded-virtual
        -Wpedantic
        -Wredundant-decls
        -Wsign-conversion
        -Wunused
    )

    # ── GCC 14+ extras ───────────────────────────────────────────────────────
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} INTERFACE
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
            -Wmissing-noreturn
            -Wmissing-format-attribute
            -Wno-global-module
            -Wno-missing-field-initializers
        )

        if(CONFLUX_GCC_SUGGEST_ATTRIBUTES)
            target_compile_options(${target} INTERFACE
                -Wsuggest-attribute=pure
                -Wsuggest-attribute=const
            )
        endif()

        # GCC 16.1 can fail while lazily deserializing already-built project CMIs
        # with diagnostics such as "failed to read compiled module cluster" and
        # "failed to load pendings".  Eager module import keeps the same module
        # graph, but avoids the lazy CMI path that is failing here.  Keep this as
        # a cache toggle so later GCC point releases can re-enable lazy imports
        # without touching source.
        if(CONFLUX_GCC_EAGER_MODULE_IMPORTS
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16")
            target_compile_options(${target} INTERFACE -fno-module-lazy)
        endif()
    endif()

    # ── Clang 18 extras ──────────────────────────────────────────────────────
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} INTERFACE
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Wno-unqualified-std-cast-call
            -Wno-missing-designated-field-initializers
        )
    endif()

    if(CONFLUX_SUPPRESS_DEPRECATION_WARNINGS)
        target_compile_options(${target} INTERFACE -Wno-deprecated-declarations)
    endif()

    # ── Sanitisers ────────────────────────────────────────────────────────────
    # hack: GCC 15.x ICE in tree_node (cp/module.cc:10037) when ASan or UBSan
    # is enabled alongside C++26 modules. CONFLUX_ENABLE_ASAN/UBSAN must be
    # left OFF for debug-gcc-stdcxx until the upstream GCC bug is resolved.
    if(CONFLUX_ENABLE_ASAN)
        target_compile_options(${target} INTERFACE
            -fsanitize=address
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(${target} INTERFACE -fsanitize=address)
    endif()

    if(CONFLUX_ENABLE_UBSAN)
        target_compile_options(${target} INTERFACE
            -fsanitize=undefined
            -fno-sanitize-recover=all)
        target_link_options(${target} INTERFACE -fsanitize=undefined)
    endif()

    if(CONFLUX_ENABLE_TSAN)
        target_compile_options(${target} INTERFACE -fsanitize=thread)
        target_link_options(${target}    INTERFACE -fsanitize=thread)
    endif()

    # ── LTO ──────────────────────────────────────────────────────────────────
    if(CONFLUX_ENABLE_LTO)
        target_compile_options(${target} INTERFACE -flto=auto)
        target_link_options(${target}    INTERFACE -flto=auto)
    endif()

    # ── PGO ──────────────────────────────────────────────────────────────────
    if(CONFLUX_PGO_GENERATE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} INTERFACE -fprofile-instr-generate)
            target_link_options(${target}    INTERFACE -fprofile-instr-generate)
        else()
            target_compile_options(${target} INTERFACE -fprofile-generate=${CONFLUX_PGO_PROFILE_DIR})
            target_link_options(${target}    INTERFACE -fprofile-generate=${CONFLUX_PGO_PROFILE_DIR})
        endif()
    elseif(NOT CONFLUX_PGO_PROFILE_DIR STREQUAL "")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} INTERFACE -fprofile-instr-use=${CONFLUX_PGO_PROFILE_DIR})
            target_link_options(${target}    INTERFACE -fprofile-instr-use=${CONFLUX_PGO_PROFILE_DIR})
        else()
            target_compile_options(${target} INTERFACE
                -fprofile-use=${CONFLUX_PGO_PROFILE_DIR}
                -fprofile-correction)
            target_link_options(${target}    INTERFACE -fprofile-use=${CONFLUX_PGO_PROFILE_DIR})
        endif()
    endif()

    # ── libFuzzer coverage instrumentation ───────────────────────────────────
    if(CONFLUX_BUILD_FUZZ)
        target_compile_options(${target} INTERFACE -fsanitize=fuzzer-no-link)
    endif()
endfunction()
