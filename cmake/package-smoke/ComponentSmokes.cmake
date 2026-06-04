function(conflux_add_component_smoke name component source)
    if(NOT TARGET "conflux::${component}")
        message(FATAL_ERROR "package smoke expected target conflux::${component}")
    endif()
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/package_smoke_${name}.cxx")
    file(WRITE "${_source}" "${_conflux_package_smoke_forbidden_surface_check}${source}")
    if(_conflux_package_smoke_linked_apis)
        add_executable("conflux_package_smoke_${name}" "${_source}")
    else()
        add_library("conflux_package_smoke_${name}" OBJECT "${_source}")
    endif()
    target_compile_features("conflux_package_smoke_${name}" PRIVATE cxx_std_23)
    conflux_apply_package_smoke_build_policy("conflux_package_smoke_${name}")
    conflux_link_package_smoke_base_targets("conflux_package_smoke_${name}")
    target_link_libraries("conflux_package_smoke_${name}" PRIVATE "conflux::${component}")
    if(_conflux_package_smoke_linked_apis)
        add_test(NAME "package-smoke/${name}" COMMAND "conflux_package_smoke_${name}")
    endif()
endfunction()

foreach(_component IN LISTS _conflux_smoke_components)
	if(_component STREQUAL "core")
		if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(core core [[
import conflux.types;
int main() {
    auto summary = conflux::build_info_summary();
    return summary.empty() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(core core [[
import conflux.types;
int main() { return 0; }
]])
			endif()
		else()
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(core core [[
#include <conflux/config.hpp>
int main() {
    auto summary = conflux::build_info_summary();
    return summary.empty() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(core core [[
#include <conflux/config.hpp>
int main() { return 0; }
]])
			endif()
		endif()
	elseif(_component STREQUAL "json")
		if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(json json [[
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
#if !CONFLUX_SURFACE_HAS_JSON
#error "conflux::json package target must publish CONFLUX_SURFACE_HAS_JSON=1"
#endif
import conflux.json;
int main() {
    auto parsed = conflux::json::parse("{\"x\":1}");
    return parsed.has_value() ? 0 : 1;
}
]])
			else()
				conflux_add_component_smoke(json json [[
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
#if !CONFLUX_SURFACE_HAS_JSON
#error "conflux::json package target must publish CONFLUX_SURFACE_HAS_JSON=1"
#endif
import conflux.json;
int main() { return 0; }
]])
			endif()
		else()
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(json json [[
#include <conflux/features.hxx>
#include <conflux/json.hpp>
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
#if !CONFLUX_SURFACE_HAS_JSON
#error "conflux::json package target must publish CONFLUX_SURFACE_HAS_JSON=1"
#endif
static_assert(conflux::HAS_JSON);
int main() {
    auto parsed = conflux::json::parse("{\"x\":1}");
    return parsed.has_value() ? 0 : 1;
}
]])
			else()
				conflux_add_component_smoke(json json [[
#include <conflux/features.hxx>
#include <conflux/json.hpp>
#if !CONFLUX_HAS_JSON
#error "conflux::json package target must publish CONFLUX_HAS_JSON=1"
#endif
#if !CONFLUX_SURFACE_HAS_JSON
#error "conflux::json package target must publish CONFLUX_SURFACE_HAS_JSON=1"
#endif
static_assert(conflux::HAS_JSON);
int main() { return 0; }
]])
			endif()
		endif()
    elseif(_component STREQUAL "http")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            if(_conflux_package_smoke_linked_apis)
                conflux_add_component_smoke(http http [[
#if !CONFLUX_SURFACE_HAS_HTTP_FACADE
#error "conflux::http package target must publish CONFLUX_SURFACE_HAS_HTTP_FACADE=1"
#endif
import conflux.http;
int main() {
    auto app = conflux::http::app();
    app.get("/health", [] { return conflux::http::text("ok"); }).name("health.check");
    return app.routes().empty() ? 1 : 0;
}
]])
            else()
                conflux_add_component_smoke(http http [[
#if !CONFLUX_SURFACE_HAS_HTTP_FACADE
#error "conflux::http package target must publish CONFLUX_SURFACE_HAS_HTTP_FACADE=1"
#endif
import conflux.http;
int main() { return 0; }
]])
            endif()
        else()
            if(_conflux_package_smoke_linked_apis)
                conflux_add_component_smoke(http http [[
#include <conflux/http.hpp>
#if !CONFLUX_SURFACE_HAS_HTTP_FACADE
#error "conflux::http package target must publish CONFLUX_SURFACE_HAS_HTTP_FACADE=1"
#endif
int main() {
    auto app = conflux::http::app();
    app.get("/health", [] { return conflux::http::text("ok"); }).name("health.check");
    return app.routes().empty() ? 1 : 0;
}
]])
            else()
                conflux_add_component_smoke(http http [[
#include <conflux/http.hpp>
#if !CONFLUX_SURFACE_HAS_HTTP_FACADE
#error "conflux::http package target must publish CONFLUX_SURFACE_HAS_HTTP_FACADE=1"
#endif
int main() { return 0; }
]])
            endif()
        endif()
	elseif(_component STREQUAL "file_io_sync")
		if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(file_io_sync file_io_sync [[
import conflux.file_io_sync;
int main() {
    auto text = conflux::file_io_sync::read_text_file_nothrow("/definitely/not/a/conflux/package-smoke-file");
    return text.has_value() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(file_io_sync file_io_sync [[
import conflux.file_io_sync;
int main() { return 0; }
]])
			endif()
		else()
			if(_conflux_package_smoke_linked_apis)
				conflux_add_component_smoke(file_io_sync file_io_sync [[
#include <conflux/file_io_sync.hpp>
int main() {
    auto text = conflux::file_io_sync::read_text_file_nothrow("/definitely/not/a/conflux/package-smoke-file");
    return text.has_value() ? 1 : 0;
}
]])
			else()
				conflux_add_component_smoke(file_io_sync file_io_sync [[
#include <conflux/file_io_sync.hpp>
int main() { return 0; }
]])
			endif()
		endif()
    elseif(_component STREQUAL "template")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(template template [[
import conflux.templates;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(template template [[
#include <conflux/template.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "dns")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(dns dns [[
import conflux.net.dns;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(dns dns [[
#include <conflux/dns.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "work")
        if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
            conflux_add_component_smoke(work work [[
import conflux.work;
int main() { return 0; }
]])
        else()
            conflux_add_component_smoke(work work [[
#include <conflux/work.hpp>
int main() { return 0; }
]])
        endif()
    elseif(_component STREQUAL "pg")
        if(CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
            if(CONFLUX_INTERFACE_MODE STREQUAL "MODULE_INTERFACE")
                conflux_add_component_smoke(pg pg [[
import conflux.pg;
int main() { return 0; }
]])
            else()
                conflux_add_component_smoke(pg pg [[
#include <conflux/pg/types.hxx>
int main() { return 0; }
]])
            endif()
        endif()
    endif()
endforeach()

if(NOT CONFLUX_PACKAGE_SMOKE_ENABLE_DB)
    set(_db_headers
        "${_conflux_install_prefix}/include/conflux/db.hxx"
        "${_conflux_install_prefix}/include/conflux/db.hpp"
        "${_conflux_install_prefix}/include/conflux/pg.hxx"
        "${_conflux_install_prefix}/include/conflux/pg.hpp")
    foreach(_db_header IN LISTS _db_headers)
        if(EXISTS "${_db_header}")
            message(FATAL_ERROR "DB-disabled package smoke found installed DB header: ${_db_header}")
        endif()
    endforeach()
    if(EXISTS "${_conflux_install_prefix}/include/conflux/pg")
        message(FATAL_ERROR
            "DB-disabled package smoke found installed DB include directory: ${_conflux_install_prefix}/include/conflux/pg")
    endif()
endif()
