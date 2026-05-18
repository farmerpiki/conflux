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
        string(TOUPPER "${CONFLUX_LTO_MODE}" _conflux_lto_mode)
        if(NOT _conflux_lto_mode MATCHES "^(AUTO|THIN|FULL)$")
            message(FATAL_ERROR
                "conflux: CONFLUX_LTO_MODE must be AUTO, THIN, or FULL "
                "(got '${CONFLUX_LTO_MODE}')")
        endif()

        set(_conflux_lto_flag "-flto=auto")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            if(_conflux_lto_mode STREQUAL "FULL")
                set(_conflux_lto_flag "-flto")
            else()
                # Clang does not support GCC's -flto=auto spelling.  ThinLTO
                # keeps optimized builds usable for large module-heavy targets.
                set(_conflux_lto_flag "-flto=thin")
            endif()
        elseif(_conflux_lto_mode STREQUAL "FULL")
            set(_conflux_lto_flag "-flto")
        elseif(_conflux_lto_mode STREQUAL "THIN")
            message(FATAL_ERROR
                "conflux: CONFLUX_LTO_MODE=THIN is only supported for Clang presets")
        endif()

        target_compile_options(${target} INTERFACE ${_conflux_lto_flag})
        target_link_options(${target}    INTERFACE ${_conflux_lto_flag})
    endif()

    # ── PGO ──────────────────────────────────────────────────────────────────
    if(CONFLUX_PGO_GENERATE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} INTERFACE -fprofile-instr-generate=${CONFLUX_PGO_PROFILE_DIR})
            target_link_options(${target}    INTERFACE -fprofile-instr-generate=${CONFLUX_PGO_PROFILE_DIR})
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
