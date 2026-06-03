if(CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER)
    if(NOT CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
        message(FATAL_ERROR
            "CONFLUX_PACKAGE_SMOKE_MIXED_MODULE_HEADER requires a MODULE_INTERFACE install tree")
    endif()

    set(_conflux_mixed_import_source "${CMAKE_CURRENT_BINARY_DIR}/mixed_import.cxx")
    set(_conflux_mixed_include_source "${CMAKE_CURRENT_BINARY_DIR}/mixed_include.cxx")
    set(_conflux_mixed_main_source "${CMAKE_CURRENT_BINARY_DIR}/mixed_main.cxx")

    set(_conflux_mixed_expected 42)
    set(_conflux_mixed_import_modules "import conflux.types;
")
    set(_conflux_mixed_import_checks [[
    std::optional<int> value{41};
    int score = value.value_or(0) + 1;
]])
    set(_conflux_mixed_include_headers [[#include <conflux/types.hpp>
]])
    set(_conflux_mixed_include_checks [[
    std::optional<int> value{57};
    int score = value.value_or(0) - 15;
]])

    if(TARGET conflux::json)
        math(EXPR _conflux_mixed_expected "${_conflux_mixed_expected} + 1")
        string(APPEND _conflux_mixed_import_modules "import conflux.json;
")
        string(APPEND _conflux_mixed_import_checks [[
    auto parsed = conflux::json::parse(R"({"x":1})");
    if (!parsed.has_value()) {
        return -1;
    }
    score += 1;
]])
        string(APPEND _conflux_mixed_include_headers "#include <conflux/json.hpp>
")
        string(APPEND _conflux_mixed_include_checks [[
#if !defined(CONFLUX_HAS_JSON) || !CONFLUX_HAS_JSON
        return -1;
#endif
    score += 1;
]])
    endif()

    if(TARGET conflux::http)
        math(EXPR _conflux_mixed_expected "${_conflux_mixed_expected} + 1")
        string(APPEND _conflux_mixed_import_modules "import conflux.http;
")
        string(APPEND _conflux_mixed_import_checks [[
    auto response = conflux::http::text("ok");
    if (response.status != 200 || response.text_body() != std::string_view{"ok"}) {
        return -2;
    }
    score += 1;
]])
        string(APPEND _conflux_mixed_include_headers "#include <conflux/http.hpp>
")
        string(APPEND _conflux_mixed_include_checks [[
    auto response = conflux::http::text("ok");
    if (response.status != 200 || response.text_body() != std::string_view{"ok"}) {
        return -2;
    }
    score += 1;
]])
    endif()

    if(CONFLUX_USE_IMPORT_STD)
        set(_conflux_mixed_import_std_prelude "import std;
")
    else()
        set(_conflux_mixed_import_std_prelude "#include <optional>
#include <string_view>
")
    endif()

    file(WRITE "${_conflux_mixed_import_source}" "${_conflux_mixed_import_std_prelude}
${_conflux_mixed_import_modules}
int conflux_mixed_imported() {
${_conflux_mixed_import_checks}    return score;
}
")

    file(WRITE "${_conflux_mixed_include_source}" "${_conflux_mixed_include_headers}
#include <optional>
#include <string_view>

int conflux_mixed_included() {
${_conflux_mixed_include_checks}    return score;
}
")

    file(WRITE "${_conflux_mixed_main_source}" "int conflux_mixed_imported();
int conflux_mixed_included();

int main() {
    return (conflux_mixed_imported() == ${_conflux_mixed_expected} && conflux_mixed_included() == ${_conflux_mixed_expected}) ? 0 : 1;
}
")

    add_executable(conflux_package_smoke_mixed_module_header
        "${_conflux_mixed_import_source}"
        "${_conflux_mixed_include_source}"
        "${_conflux_mixed_main_source}")
    target_compile_features(conflux_package_smoke_mixed_module_header PRIVATE cxx_std_23)
    conflux_apply_package_smoke_build_policy(conflux_package_smoke_mixed_module_header)
    conflux_link_package_smoke_base_targets(conflux_package_smoke_mixed_module_header)
    foreach(_component IN LISTS _conflux_find_components)
        if(TARGET "conflux::${_component}")
            target_link_libraries(conflux_package_smoke_mixed_module_header PRIVATE "conflux::${_component}")
        endif()
    endforeach()
    add_test(NAME package-smoke/mixed-module-header COMMAND conflux_package_smoke_mixed_module_header)
endif()
