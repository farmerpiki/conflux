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
        -Wshadow
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
            -Wsuggest-attribute=pure
            -Wsuggest-attribute=const
            -Wmissing-noreturn
            -Wmissing-format-attribute
            -Wno-global-module
        )
    endif()

    # ── Clang 18 extras ──────────────────────────────────────────────────────
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} INTERFACE
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
            -Wno-unqualified-std-cast-call
        )
    endif()

    if(CONFLUX_SUPPRESS_DEPRECATION_WARNINGS)
        target_compile_options(${target} INTERFACE -Wno-deprecated-declarations)
    endif()

    # ── Sanitisers ────────────────────────────────────────────────────────────
    if(CONFLUX_ENABLE_ASAN)
        target_compile_options(${target} INTERFACE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target}    INTERFACE -fsanitize=address)
    endif()

    if(CONFLUX_ENABLE_UBSAN)
        target_compile_options(${target} INTERFACE -fsanitize=undefined)
        target_link_options(${target}    INTERFACE -fsanitize=undefined)
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

    # ── libFuzzer coverage instrumentation ───────────────────────────────────
    if(CONFLUX_BUILD_FUZZ)
        target_compile_options(${target} INTERFACE -fsanitize=fuzzer-no-link)
    endif()
endfunction()
