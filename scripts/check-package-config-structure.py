#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import sys
from collections import Counter
from pathlib import Path


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read(path: str) -> str:
    return Path(path).read_text(encoding="utf-8")


def cmake_function_body(text: str, signature: str) -> str:
    try:
        return text.split(signature, 1)[1].split("endfunction()", 1)[0]
    except IndexError:
        fail(f"missing CMake function body: {signature}")


def shell_semicolon_list_var(text: str, name: str) -> set[str]:
    match = re.search(rf"^{re.escape(name)}=\"([^\"]*)\"", text, re.MULTILINE)
    if match is None:
        fail(f"missing shell semicolon-list variable: {name}")
    return {item for item in match.group(1).split(";") if item}


def shell_policy_variables(text: str) -> set[str]:
    return set(re.findall(r"^(forbid_[A-Za-z0-9_]+)=\"[^\"]*\"", text, re.MULTILINE))


def shell_semicolon_flag_value(text: str, flag: str) -> set[str]:
    match = re.search(rf"{re.escape(flag)}\s+'([^']*)'", text)
    if match is None:
        fail(f"missing shell semicolon-list flag: {flag}")
    return {item for item in match.group(1).split(";") if item}


def shell_flag_present(text: str, flag: str) -> bool:
    return re.search(rf"(^|[ \t\\]){re.escape(flag)}([ \t\\\n]|$)", text) is not None


def shell_pkg_config_exists_probes(text: str) -> set[str]:
    return set(re.findall(r"pkg-config\s+--exists\s+([A-Za-z0-9_.+-]+)", text))


def shell_cmake_definitions(text: str) -> dict[str, str]:
    return dict(re.findall(r"-D([A-Za-z0-9_]+)=([^ \t\n\\]+)", text))


def append_set_delta_errors(
    errors: list[str],
    expected: set[str],
    actual: set[str],
    missing_message: str,
    extra_message: str,
) -> None:
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing:
        errors.append(missing_message + ";".join(missing))
    if extra:
        errors.append(extra_message + ";".join(extra))


def check_marker_order(text: str, markers: list[str], diagnostic: str) -> None:
    positions = [text.find(marker) for marker in markers]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        fail(diagnostic)


def check_no_legacy_stdsimd_option() -> None:
    roots = [
        Path("CMakeLists.txt"),
        Path("CMakePresets.json"),
        Path("cmake"),
        Path("scripts"),
        Path("docs"),
        Path("tests"),
        Path("examples"),
        Path("benchmarks"),
        Path("proposals"),
    ]
    legacy_name = "CONFLUX_JSON" + "_USE_STDSIMD"
    hits: list[str] = []
    for root in roots:
        paths = [root] if root.is_file() else root.rglob("*")
        for path in paths:
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for line_no, line in enumerate(text.splitlines(), start=1):
                if legacy_name in line:
                    hits.append(f"{path}:{line_no}: {line.strip()}")
    if hits:
        fail("\n".join(hits))


def check_no_explicit_build_parallelism() -> None:
    roots = [
        Path("build-all.sh"),
        Path("scripts"),
        Path("docs"),
        Path("benchmarks"),
    ]
    command_pattern = re.compile(r"cmake\s+--build\b.*(?:\s-j(?:\s|[0-9]|$)|\s--parallel(?:\s|=|$))")
    helper_pattern = re.compile(r"f?['\"]-j\{?jobs\}?['\"]")
    prose_pattern = re.compile(r"\bBuild with -j[0-9]*\b")
    hits: list[str] = []
    for root in roots:
        paths = [root] if root.is_file() else root.rglob("*")
        for path in paths:
            if not path.is_file() or "benchmarks/corpus" in path.as_posix():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for line_no, line in enumerate(text.splitlines(), start=1):
                if command_pattern.search(line) or helper_pattern.search(line) or prose_pattern.search(line):
                    hits.append(f"{path}:{line_no}: explicit build parallelism is not allowed: {line.strip()}")
    if hits:
        fail("\n".join(hits))


def check_provider_policy_scenarios_are_isolated() -> None:
    text = read("scripts/check-provider-policy-matrix.sh")
    required_flags = {
        "-DCONFLUX_BUILD_TESTS=OFF",
        "-DCONFLUX_BUILD_EXAMPLES=OFF",
        "-DCONFLUX_BUILD_BENCHMARKS=OFF",
        "-DCONFLUX_FETCH_TEST_DEPS=OFF",
    }
    errors: list[str] = []
    scenario_pattern = re.compile(
        r'(?P<var>[A-Za-z0-9_]+_dir)="\$\((?P<body>run_configure(?:_no_system_pc|_no_argon2_pc)?\b.*?)\)"',
        re.DOTALL,
    )
    scenarios = list(scenario_pattern.finditer(text))
    if not scenarios:
        errors.append("provider-policy matrix must contain isolated configure scenarios")
    for match in scenarios:
        body = match.group("body")
        missing = sorted(flag for flag in required_flags if flag not in body)
        if missing:
            errors.append(f"{match.group('var')} missing provider-policy isolation flags: {';'.join(missing)}")

    try:
        module_probe = text.split('cmake -S "$root" -B "$module_probe_dir"', 1)[1].split('>"$module_probe_log"', 1)[0]
    except IndexError:
        errors.append("provider-policy module-interface probe is missing")
    else:
        missing = sorted(flag for flag in required_flags if flag not in module_probe)
        if missing:
            errors.append(f"module-interface provider-policy probe missing isolation flags: {';'.join(missing)}")

    if errors:
        fail("\n".join(errors))


def check_run_build_artifact_root_examples_are_declared() -> None:
    runner = read("scripts/run-build-artifact.sh")
    declared = declared_cmake_targets(["examples/CMakeLists.txt"])
    try:
        body = runner.split("valid_root_example() {", 1)[1].split("env_args=()", 1)[0]
    except IndexError:
        fail("missing run-build-artifact root example allowlist")
    allowed = re.findall(r"\b(conflux_[A-Za-z0-9_]+)\b", body)
    errors: list[str] = []
    if not allowed:
        errors.append("run-build-artifact root example allowlist must be non-empty")
    duplicates = sorted(name for name, count in Counter(allowed).items() if count > 1)
    if duplicates:
        errors.append(f"run-build-artifact root example allowlist contains duplicates: {';'.join(duplicates)}")
    stale = sorted(name for name in allowed if name not in declared)
    if stale:
        errors.append(f"run-build-artifact root example allowlist contains undeclared example targets: {';'.join(stale)}")
    if errors:
        fail("\n".join(errors))


def check_compile_time_bench_defaults() -> None:
    bench = read("scripts/compile_time_bench.py")
    declared_targets = declared_cmake_targets([
        "CMakeLists.txt",
        "examples/CMakeLists.txt",
        "cmake/ConfluxInterfaceMode.cmake",
    ])
    cases = re.findall(
        r'Case\("(?P<name>[^"]+)",\s*"(?P<target>[^"]+)"(?:,\s*"(?P<source>[^"]+)")?',
        bench,
    )
    errors: list[str] = []
    if not cases:
        errors.append("compile-time benchmark default cases must be non-empty")
    names = [case[0] for case in cases]
    targets = [case[1] for case in cases]
    duplicate_names = sorted(name for name, count in Counter(names).items() if count > 1)
    if duplicate_names:
        errors.append(f"compile-time benchmark duplicate case names: {';'.join(duplicate_names)}")
    duplicate_targets = sorted(target for target, count in Counter(targets).items() if count > 1)
    if duplicate_targets:
        errors.append(f"compile-time benchmark duplicate default targets: {';'.join(duplicate_targets)}")
    missing_targets = sorted(target for target in targets if target not in declared_targets)
    if missing_targets:
        errors.append(f"compile-time benchmark default targets are not declared by CMake: {';'.join(missing_targets)}")
    missing_sources = sorted(source for _, _, source in cases if source and not Path(source).is_file())
    if missing_sources:
        errors.append(f"compile-time benchmark incremental sources are missing: {';'.join(missing_sources)}")
    if errors:
        fail("\n".join(errors))


def check_header_bridge_optional_inputs() -> None:
    text = read("cmake/ConfluxInterfaceMode.cmake")
    if "function(conflux_append_optional_bridge_inputs args_out roots_out)" not in text:
        fail("header bridge optional examples/tests/benchmarks inputs must be centralized")
    checks = {
        "--examples-src": "CONFLUX_BUILD_EXAMPLES",
        "--tests-src": "CONFLUX_BUILD_TESTS",
        "--benchmarks-src": "CONFLUX_BUILD_BENCHMARKS",
    }
    lines = text.splitlines()
    failures: list[str] = []
    for index, line in enumerate(lines):
        for needle, flag in checks.items():
            if needle not in line:
                continue
            window = "\n".join(lines[max(0, index - 4):index + 1])
            if f"if({flag})" not in window:
                failures.append(f"{needle} is not guarded by if({flag}) near line {index + 1}")
    if failures:
        fail("\n".join(failures))


def check_header_http_impls_do_not_pull_json() -> None:
    text = read("cmake/ConfluxInterfaceMode.cmake")
    if "function(conflux_bridge_link_header_dependencies" in text:
        fail("header bridge must not keep an unused all-provider dependency linker")
    if "function(conflux_link_header_impl_for_source_id" in text:
        fail("linked header examples must declare implementation deps explicitly")
    if "cmake_parse_arguments(CONFLUX_HEADER_EXAMPLE" not in text:
        fail("header example registration must parse explicit implementation deps")
    http_impl_body = cmake_function_body(
        text,
        "function(conflux_link_header_http_impls target)",
    )
    if "conflux_header_impl_json" in http_impl_body:
        fail("generic header HTTP impl closure must not pull conflux_header_impl_json")
    if (
        "conflux_header_impl_http_core" in http_impl_body
        and "router($|[.:])" in text
        and "conflux_header_impl_http_static" not in http_impl_body
    ):
        fail("generic header HTTP impl closure must keep conflux_header_impl_http_static while router_impl owns serve_static")

    json_example_sources = {
        "examples/advanced/explicit_offload",
        "examples/advanced/http_client_json",
        "examples/advanced/manual_json_members",
        "examples/advanced/production_showcase",
        "examples/public/hello",
        "examples/public/middleware",
        "examples/quickstart/json_crud",
        "examples/quickstart/json_reflect_crud",
        "examples/quickstart/openapi",
        "examples/quickstart/postgres_json",
        "examples/public/static",
        "examples/public/gzip",
    }
    add_examples_body = cmake_function_body(
        text,
        "function(conflux_add_header_examples_from_source_ids)",
    )
    dual_match = re.search(
        r"conflux_add_header_example_from_id\(\s*"
        r"conflux_dual\s+examples/advanced/dual(?P<body>.*?)\)",
        add_examples_body,
        re.DOTALL,
    )
    if dual_match is None:
        fail("header examples must register the dual example")
    dual_tokens = set(dual_match.group("body").split())
    missing_dual_impls = {
        "conflux_header_impl_http_client",
        "conflux_header_impl_http_proxy",
    } - dual_tokens
    if missing_dual_impls:
        fail(
            "dual header example must declare HTTP client implementation deps explicitly: "
            + ";".join(sorted(missing_dual_impls)),
        )
    call_pattern = re.compile(
        r"conflux_add_header_example_from_id\((.*?)\)",
        re.DOTALL,
    )
    missing: list[str] = []
    for match in call_pattern.finditer(add_examples_body):
        call = match.group(1)
        tokens = call.split()
        if len(tokens) < 2:
            continue
        source_id = tokens[1]
        if source_id in json_example_sources and "conflux_header_impl_json" not in tokens:
            missing.append(source_id)
    if missing:
        fail(
            "header JSON examples must declare conflux_header_impl_json explicitly: "
            + ";".join(sorted(missing)),
        )


def check_header_impl_lists_have_no_duplicates() -> None:
    files = [
        Path("cmake/ConfluxHeaderInterface.cmake"),
        Path("cmake/ConfluxInterfaceMode.cmake"),
    ]
    call_pattern = re.compile(
        r"\b(conflux_header_public_component(?:_by_export)?|conflux_add_header_example_from_id)\((.*?)\)",
        re.DOTALL,
    )
    keywords = {
        "COMPILE_DEFINITIONS",
        "HPP_TOP_LEVEL",
        "HTTP_IMPLS",
        "IMPLS",
        "LINKS",
        "MODULE_PREFIXES",
        "NO_PACKAGE",
    }
    failures: list[str] = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        for match in call_pattern.finditer(text):
            tokens = match.group(2).split()
            impls: list[str] = []
            in_impls = False
            for token in tokens:
                if token == "IMPLS":
                    in_impls = True
                    continue
                if token in keywords:
                    in_impls = False
                    continue
                if in_impls:
                    impls.append(token)
            duplicates = sorted(name for name, count in Counter(impls).items() if count > 1)
            if duplicates:
                line_no = text.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{path}:{line_no}: duplicate header implementation entries: "
                    + ";".join(duplicates),
                )
    if failures:
        fail("\n".join(failures))


def check_header_source_ids_exist() -> None:
    text = read("cmake/ConfluxInterfaceMode.cmake")
    source_ids: set[str] = set()
    call_patterns = [
        r"\bconflux_add_header_example_from_id\(\s+\S+\s+"
        r"((?:examples|tests|benchmarks|src)/[^\s\)]+)",
        r"\bconflux_add_header_compile_fail_test\(\s+\S+\s+"
        r"((?:examples|tests|benchmarks|src)/[^\s\)]+)",
    ]
    for pattern in call_patterns:
        source_ids.update(re.findall(pattern, text))

    list_pattern = re.compile(
        r"\b(?:set|list\(APPEND)\s*\(\s*"
        r"(?:_conflux_header_test_source_ids|_conflux_header_benchmark_source_ids)"
        r"(?P<body>.*?)\)",
        re.DOTALL,
    )
    for match in list_pattern.finditer(text):
        source_ids.update(
            re.findall(r"\b(?:examples|tests|benchmarks|src)/[A-Za-z0-9_/-]+", match.group("body")),
        )

    missing = [
        source_id
        for source_id in sorted(source_ids)
        if not Path(f"{source_id}.cxx").is_file()
    ]
    if missing:
        fail("header bridge source ids are missing backing .cxx files: " + ";".join(missing))


def check_header_support_components_are_limited() -> None:
    text = read("cmake/ConfluxHeaderInterface.cmake")
    body = cmake_function_body(
        text,
        "function(conflux_header_support_component target export_name)",
    )
    allowed_exports = support_component_exports_from_registry() | generated_header_support_exports()
    allowed = {
        ("conflux_headers", "headers"),
        ("conflux_header_impl", "header_impl"),
    }
    calls = set()
    for match in re.finditer(r"\bconflux_header_support_component\((.*?)\)", text, re.DOTALL):
        if match.start() < text.find(body) + len(body):
            continue
        tokens = match.group(1).split()
        if len(tokens) != 2:
            line_no = text.count("\n", 0, match.start()) + 1
            fail(f"cmake/ConfluxHeaderInterface.cmake:{line_no}: malformed header support component registration")
        if tokens[1] not in allowed_exports:
            line_no = text.count("\n", 0, match.start()) + 1
            fail(f"cmake/ConfluxHeaderInterface.cmake:{line_no}: header support component `{tokens[1]}` is neither a support registry export nor a generated-header support export")
        calls.add((tokens[0], tokens[1]))
    unknown = sorted(calls - allowed)
    missing = sorted(allowed - calls)
    errors: list[str] = []
    if unknown:
        errors.append(
            "header support components must stay limited to generated-header support targets: "
            + ";".join(f"{target}|{export}" for target, export in unknown),
        )
    if missing:
        errors.append(
            "missing generated-header support component registrations: "
            + ";".join(f"{target}|{export}" for target, export in missing),
        )
    if errors:
        fail("\n".join(errors))


def check_header_public_components_use_registry_exports() -> None:
    text = read("cmake/ConfluxHeaderInterface.cmake")
    public_exports = public_component_exports_from_registry()
    by_export_body = cmake_function_body(
        text,
        "macro(conflux_header_public_component_by_export export_name)",
    )
    body_start = text.find(by_export_body)
    body_end = body_start + len(by_export_body)
    failures: list[str] = []
    for match in re.finditer(r"\bconflux_header_public_component\(", text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        if text[line_start:match.start()].strip() in {"macro("}:
            continue
        if body_start <= match.start() < body_end:
            continue
        line_no = text.count("\n", 0, match.start()) + 1
        failures.append(
            f"cmake/ConfluxHeaderInterface.cmake:{line_no}: package header components must use conflux_header_public_component_by_export",
        )
    call_pattern = re.compile(r"\bconflux_header_public_component_by_export\((.*?)\)", re.DOTALL)
    for match in call_pattern.finditer(text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        if text[line_start:match.start()].strip() in {"macro("}:
            continue
        tokens = match.group(1).split()
        line_no = text.count("\n", 0, match.start()) + 1
        if not tokens:
            failures.append(
                f"cmake/ConfluxHeaderInterface.cmake:{line_no}: empty header public component registration",
            )
            continue
        export_name = tokens[0]
        if export_name not in public_exports:
            failures.append(
                f"cmake/ConfluxHeaderInterface.cmake:{line_no}: header public component `{export_name}` is not a public registry export",
            )
    if failures:
        fail("\n".join(failures))


def check_header_interface_contracts() -> None:
    checks = {
        "cmake/ConfluxOptions.cmake": {
            "CONFLUX_PACKAGE_SMOKE_COMPONENTS": "missing package smoke component cache variable",
            "add_test(NAME build/package-config-install-tree": "missing installed-prefix package smoke CTest guard",
            "CONFLUX_BUILD_PACKAGE_TESTS": "missing package-only CTest option",
            "CONFLUX_HEADER_FAST_COMPILE": "missing header fast-compile option",
            "CONFLUX_HEADER_LINK_EXAMPLES": "missing opt-in linked header examples option",
            "CONFLUX_HEADER_LINK_SMOKE": "missing opt-in linked header smoke option",
            "CONFLUX_RUN_HEADER_COMPONENT_SMOKE": "missing opt-in full header component smoke option",
            'CONFLUX_INTERFACE_MODE STREQUAL "HEADER_INTERFACE"': "API surface definitions must handle header mode separately",
            "CONFLUX_WANT_HTTP_POLICY": "header API surface macros must derive from resolved component flags",
            "CONFLUX_RUN_INSTALL_TREE_SMOKE": "missing opt-in install-tree smoke CTest option",
            "add_test(NAME build/install-tree-smoke": "missing install-tree smoke CTest guard",
            "set(CMAKE_CXX_SCAN_FOR_MODULES OFF)": "HEADER_INTERFACE must disable CMake module scanning",
        },
        "cmake/ConfluxInterfaceMode.cmake": {
            "CXX_SCAN_FOR_MODULES OFF": "header generated targets must disable module scanning",
            "CONFLUX_HEADER_FAST_COMPILE": "header generated targets must honor fast-compile option",
            "CONFLUX_HEADER_LINK_EXAMPLES": "header examples must keep implementation linking opt-in",
            "conflux_add_header_link_smoke_targets": "header mode must expose a linked smoke target",
            "header/link-smoke-http": "header linked HTTP smoke must be registered with CTest",
            "conflux_header_impl_json": "header implementation sources must be split by component",
            "COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-O0": "header generated targets must override release optimization for fast compile",
        },
        "scripts/check-header-first-contact-smoke.sh": {
            "conflux_header_smoke_api_surface_curated": "first-contact header smoke must build only the curated API surface target",
        },
        "scripts/check-header-component-smoke.sh": {
            "CONFLUX_HEADER_COMPONENT_SMOKE_BUILD_ROOT": "full header component smoke must remain separately configurable",
        },
        "cmake/conflux-config.cmake.in": {
            "@PACKAGE_INIT@": "package config must use PACKAGE_INIT",
            "include(CMakeFindDependencyMacro)": "package config must include CMakeFindDependencyMacro",
            "set(CONFLUX_RUNTIME_REQUIRES_LIBURING": "package config must expose runtime liburing status",
            "foreach(_conflux_component IN LISTS conflux_FIND_COMPONENTS)": "package config must validate requested components",
            "check_required_components(conflux)": "package config must call check_required_components(conflux)",
            "conflux::conflux": "package config must provide the canonical umbrella alias when available",
            '"${PACKAGE_PREFIX_DIR}/include"': "module-interface package targets must expose installed header includes",
            '"${PACKAGE_PREFIX_DIR}/include/conflux/modules"': "module-interface package targets must expose installed module support includes",
        },
    }
    errors: list[str] = []
    for path, markers in checks.items():
        text = read(path)
        errors.extend(message for marker, message in markers.items() if marker not in text)
    interface_text = read("cmake/ConfluxInterfaceMode.cmake")
    for line in interface_text.splitlines():
        if "target_link_libraries" not in line or "conflux_headers" not in line:
            continue
        if "PkgConfig::LIBURING" in line:
            errors.append("header support target must not leak liburing into every header package component")
        if "PkgConfig::XXHASH" in line:
            errors.append("header support target must not leak xxhash into every header package component")

    data = json.loads(read("CMakePresets.json"))
    configure_presets = {
        preset["name"]: preset
        for preset in data.get("configurePresets", [])
        if isinstance(preset.get("name"), str)
    }
    build_presets = {
        preset["name"]: preset
        for preset in data.get("buildPresets", [])
        if isinstance(preset.get("name"), str)
    }
    release_header = configure_presets.get("release-header-artifacts")
    if release_header is None:
        errors.append("missing release-header-artifacts preset")
    else:
        feature_set = release_header.get("cacheVariables", {}).get("CONFLUX_FEATURE_SET")
        if feature_set != "release-json":
            errors.append("release-header-artifacts must pin release-json feature set")
    release_clang = build_presets.get("release-clang-libcxx")
    if release_clang is None or release_clang.get("configurePreset") != "release-clang-libcxx":
        errors.append("missing release-clang-libcxx build preset")
    if errors:
        fail("\n".join(sorted(errors)))


def check_cmake_preset_names_unique() -> None:
    data = json.loads(read("CMakePresets.json"))
    errors: list[str] = []
    for group in ("configurePresets", "buildPresets", "testPresets"):
        names: list[str] = []
        for index, preset in enumerate(data.get(group, []), start=1):
            name = preset.get("name")
            if not isinstance(name, str) or not name:
                errors.append(f"{group}[{index}] has missing or non-string name")
                continue
            names.append(name)
        duplicates = sorted(name for name, count in Counter(names).items() if count > 1)
        if duplicates:
            errors.append(f"duplicate {group} names: {';'.join(duplicates)}")
    if errors:
        fail("\n".join(errors))


def check_cmake_preset_references() -> None:
    data = json.loads(read("CMakePresets.json"))
    configure = {
        preset["name"]
        for preset in data.get("configurePresets", [])
        if isinstance(preset.get("name"), str)
    }
    errors: list[str] = []
    for group in ("buildPresets", "testPresets"):
        for preset in data.get(group, []):
            name = preset.get("name")
            configure_preset = preset.get("configurePreset")
            if not isinstance(name, str) or not name:
                continue
            if not isinstance(configure_preset, str) or not configure_preset:
                errors.append(f"{group} preset {name} must set configurePreset")
                continue
            if configure_preset not in configure:
                errors.append(
                    f"{group} preset {name} references missing configurePreset: {configure_preset}",
                )
            if group == "buildPresets" and name != configure_preset:
                errors.append(
                    f"build preset {name} must match configurePreset {configure_preset}",
                )
    if errors:
        fail("\n".join(errors))


def cmake_test_cmake_paths() -> list[Path]:
    return [
        Path("tests/CMakeLists.txt"),
        *sorted(Path("tests").glob("*.cmake")),
        Path("cmake/ConfluxOptions.cmake"),
        Path("cmake/ConfluxInterfaceMode.cmake"),
        Path("cmake/package-smoke/CMakeLists.txt"),
        *sorted(Path("cmake/package-smoke").glob("*.cmake")),
        Path("fuzz/CMakeLists.txt"),
    ]


def cmake_test_names() -> set[str]:
    paths = cmake_test_cmake_paths()
    names: set[str] = set()
    pattern = re.compile(r"add_test\s*\(\s*NAME\s+([^\s\)]+)", re.MULTILINE)
    for path in paths:
        if not path.exists():
            continue
        for match in pattern.finditer(path.read_text(encoding="utf-8")):
            name = match.group(1).strip("'\"")
            if "$" not in name:
                names.add(name)
    return names


def cmake_test_labels() -> set[str]:
    labels: set[str] = set()
    quoted_pattern = re.compile(r'LABELS\s+"([^"]+)"')
    word_pattern = re.compile(r"LABELS\s+([A-Za-z0-9_;-]+)")
    for path in cmake_test_cmake_paths():
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        for match in quoted_pattern.finditer(text):
            labels.update(label for label in match.group(1).split(";") if "$" not in label)
        for match in word_pattern.finditer(text):
            labels.update(label for label in match.group(1).split(";") if "$" not in label)
    return labels


def check_test_preset_filters() -> None:
    data = json.loads(read("CMakePresets.json"))
    tests = cmake_test_names()
    labels = cmake_test_labels()
    errors: list[str] = []
    for preset in data.get("testPresets", []):
        name = preset.get("name")
        include = preset.get("filter", {}).get("include", {})
        test_name = include.get("name")
        if test_name is not None:
            if not isinstance(test_name, str) or not test_name:
                errors.append(f"test preset {name} has a malformed exact name filter")
            elif test_name not in tests:
                errors.append(f"test preset {name} filters unknown CTest name: {test_name}")
        label = include.get("label")
        if label is None:
            continue
        if not isinstance(label, str) or not label:
            errors.append(f"test preset {name} has a malformed label filter")
            continue
        if label not in labels:
            errors.append(f"test preset {name} filters unknown CTest label: {label}")
    if errors:
        fail("\n".join(errors))


def check_install_smoke_presets() -> None:
    data = json.loads(read("CMakePresets.json"))
    configure = {preset["name"]: preset for preset in data.get("configurePresets", [])}
    build = {preset["name"]: preset for preset in data.get("buildPresets", [])}
    test = {preset["name"]: preset for preset in data.get("testPresets", [])}
    public_components = public_component_exports_from_registry()
    required = [
        "release-core-install-smoke",
        "release-json-install-smoke",
        "release-http-api-install-smoke",
        "release-header-artifacts-install-smoke",
    ]
    errors: list[str] = []
    for name in required:
        if name not in configure:
            errors.append(f"missing configure preset: {name}")
        else:
            components = configure[name].get("cacheVariables", {}).get("CONFLUX_PACKAGE_SMOKE_COMPONENTS")
            if not isinstance(components, str) or not components:
                errors.append(f"install-smoke preset {name} must set non-empty CONFLUX_PACKAGE_SMOKE_COMPONENTS")
            else:
                unknown = sorted(component for component in components.split(";") if component not in public_components)
                if unknown:
                    errors.append(
                        f"install-smoke preset {name} requests non-public package smoke components: {';'.join(unknown)}",
                    )
        if build.get(name, {}).get("configurePreset") != name:
            errors.append(f"missing build preset mapped to configure preset: {name}")
        test_preset = test.get(name)
        if test_preset is None:
            errors.append(f"missing test preset: {name}")
            continue
        if test_preset.get("configurePreset") != name:
            errors.append(f"test preset {name} must point at configure preset {name}")
        include = test_preset.get("filter", {}).get("include", {})
        if include.get("name") != "build/install-tree-smoke":
            errors.append(f"test preset {name} must filter to build/install-tree-smoke")
        if test_preset.get("execution", {}).get("noTestsAction") != "error":
            errors.append(f"test preset {name} must set noTestsAction=error")
    if errors:
        fail("\n".join(errors))


def shell_array_values(text: str, name: str) -> list[str]:
    match = re.search(rf"^{re.escape(name)}=\((?P<body>.*?)^\)", text, re.MULTILINE | re.DOTALL)
    if match is None:
        fail(f"missing shell array: {name}")
    values: list[str] = []
    for line in match.group("body").splitlines():
        item = line.strip()
        if not item or item.startswith("#"):
            continue
        values.append(item.strip("'\""))
    return values


def shell_default_array_values(text: str, name: str, env_name: str) -> list[str]:
    match = re.search(
        rf"^{re.escape(name)}=\(\$\{{{re.escape(env_name)}:-(?P<body>[^}}]*)\}}\)",
        text,
        re.MULTILINE,
    )
    if match is None:
        fail(f"missing shell default array: {name}")
    return [item for item in match.group("body").split() if item]


def shell_string_assignment(text: str, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}=([^\n]+)$", text, re.MULTILINE)
    if match is None:
        fail(f"missing shell string assignment: {name}")
    return match.group(1).strip().strip("'\"")


def shell_default_string_words(text: str, name: str, env_name: str) -> list[str]:
    match = re.search(
        rf'^{re.escape(name)}="\$\{{{re.escape(env_name)}:-(?P<body>[^}}]*)\}}"',
        text,
        re.MULTILINE,
    )
    if match is None:
        fail(f"missing shell default string: {name}")
    return [item for item in match.group("body").split() if item]


def declared_cmake_targets(paths: list[str]) -> set[str]:
    text = "\n".join(read(path) for path in paths)
    targets = set(
        re.findall(
            r"\b(?:add_executable|add_library|add_custom_target|conflux_add_recordable_benchmark)"
            r"\((conflux_[A-Za-z0-9_]+)\b",
            text,
        ),
    )
    return {target for target in targets if "$" not in target}


def configure_preset_names() -> set[str]:
    data = json.loads(read("CMakePresets.json"))
    return {
        preset["name"]
        for preset in data.get("configurePresets", [])
        if isinstance(preset.get("name"), str)
    }


def check_matrix_script_presets() -> None:
    configure = configure_preset_names()
    matrix_scripts = {
        "scripts/run-sanitizer-matrix.sh": "sanitizer",
        "scripts/run-perf-matrix.sh": "perf",
    }
    errors: list[str] = []
    for path, label in matrix_scripts.items():
        presets = shell_array_values(read(path), "MATRIX")
        if not presets:
            errors.append(f"{path}: {label} matrix preset list must be non-empty")
            continue
        duplicates = sorted(name for name, count in Counter(presets).items() if count > 1)
        if duplicates:
            errors.append(f"{path}: duplicate {label} matrix presets: {';'.join(duplicates)}")
        unknown = sorted(name for name in presets if name not in configure)
        if unknown:
            errors.append(f"{path}: {label} matrix presets missing from CMakePresets.json: {';'.join(unknown)}")
    if errors:
        fail("\n".join(errors))


def check_build_all_presets() -> None:
    configure = configure_preset_names()
    presets = shell_array_values(read("build-all.sh"), "PRESETS")
    errors: list[str] = []
    if not presets:
        errors.append("build-all.sh: PRESETS must be non-empty")
    duplicates = sorted(name for name, count in Counter(presets).items() if count > 1)
    if duplicates:
        errors.append(f"build-all.sh: duplicate presets: {';'.join(duplicates)}")
    unknown = sorted(name for name in presets if name not in configure)
    if unknown:
        errors.append(f"build-all.sh: presets missing from CMakePresets.json: {';'.join(unknown)}")
    if errors:
        fail("\n".join(errors))


def check_script_default_presets() -> None:
    configure = configure_preset_names()
    default_sets = {
        "scripts/perf_patch_sweep.sh": shell_default_array_values(
            read("scripts/perf_patch_sweep.sh"),
            "presets",
            "PERF_PATCH_PRESETS",
        ),
        "scripts/json_perf_build_profiles.sh": shell_default_array_values(
            read("scripts/json_perf_build_profiles.sh"),
            "profiles",
            "JSON_PERF_PROFILES",
        ),
        "scripts/bench_record.sh": shell_default_string_words(
            read("scripts/bench_record.sh"),
            "BENCH_PRESETS",
            "BENCH_PRESET",
        ),
    }

    run_conditions = shell_default_array_values(
        read("scripts/json_perf_run_conditions.sh"),
        "profiles",
        "JSON_PERF_PROFILES",
    )
    default_sets["scripts/json_perf_run_conditions.sh"] = [
        f"release-{profile}" for profile in run_conditions
    ] + [
        f"pgo-use-{profile}" for profile in run_conditions
    ]

    fuzz_preset = shell_string_assignment(read("scripts/run-fuzz-smoke.sh"), "PRESET")
    default_sets["scripts/run-fuzz-smoke.sh"] = [fuzz_preset]

    errors: list[str] = []
    for path, presets in default_sets.items():
        if not presets:
            errors.append(f"{path}: default preset list must be non-empty")
            continue
        duplicates = sorted(name for name, count in Counter(presets).items() if count > 1)
        if duplicates:
            errors.append(f"{path}: duplicate default presets: {';'.join(duplicates)}")
        unknown = sorted(name for name in presets if name not in configure)
        if unknown:
            errors.append(f"{path}: default presets missing from CMakePresets.json: {';'.join(unknown)}")
    if errors:
        fail("\n".join(errors))


def check_preset_build_dir_usage_contracts() -> None:
    checks = {
        "scripts/check-changed-cxx.sh": {
            'cmake-preset-build-dir.py" "${ROOT_DIR}" debug-clang-libcxx': "changed-C++ hygiene script must resolve clang-tidy build dir from CMake presets",
            'cmake-preset-build-dir.py" "${ROOT_DIR}" debug-gcc-stdcxx': "changed-C++ hygiene script must validate debug-gcc preset",
        },
        "scripts/run-ctest.sh": {
            'python3 scripts/cmake-preset-build-dir.py "$PWD" "$1"': "run-ctest must validate build profiles through CMake presets",
            'expected_dir="$(python3 scripts/cmake-preset-build-dir.py "$PWD" "$profile")"': "run-ctest must compare absolute test dirs to preset-derived build dirs",
            "refusing unsupported build profile": "run-ctest must reject unsupported build profiles",
        },
        "scripts/run-sanitizer-matrix.sh": {
            'cmake-preset-build-dir.py" "$SOURCE_DIR" "$preset"': "sanitizer matrix must validate matrix presets through CMake presets",
            "unknown sanitizer matrix preset": "sanitizer matrix must reject --only presets outside the matrix policy",
            "no sanitizer presets selected by --only filter": "sanitizer matrix must reject empty --only selections",
        },
        "scripts/run-perf-matrix.sh": {
            'cmake-preset-build-dir.py" "$SOURCE_DIR" "$preset"': "perf matrix must validate matrix presets through CMake presets",
            "unknown perf matrix preset": "perf matrix must reject --only presets outside the matrix policy",
            "no perf presets selected by --only filter": "perf matrix must reject empty --only selections",
        },
        "build-all.sh": {
            'DEBUG_CLANG_BUILD_DIR="$(python3 scripts/cmake-preset-build-dir.py "${PWD}" debug-clang-libcxx)"': "build-all must use resolved debug-clang build dir for LSP symlink",
        },
        "scripts/perf_patch_sweep.sh": {
            'cmake-preset-build-dir.py "$repo_root" "$1"': "perf patch sweep must resolve build dirs from CMake presets",
        },
        "scripts/stage-release-artifacts.sh": {
            'cmake-preset-build-dir.py" "$root" "$preset"': "release artifact staging must resolve default build dir from CMake presets",
        },
        "scripts/bench_record.sh": {
            'select(startswith("debug-"))': "benchmark recorder debug cleanup must derive debug presets from CMakePresets.json",
            'cmake-preset-build-dir.py" "$REPO_ROOT" "$preset"': "benchmark recorder debug cleanup must resolve build dirs from CMake presets",
            'validate_bench_preset "$preset"': "benchmark recorder must validate configured benchmark presets",
        },
        "scripts/json_perf_build_profiles.sh": {
            'validate_profile_preset "$src" "$profile"': "JSON perf profile builder must validate configured presets",
            'validate_profile_preset "$src" "pgo-use-${profile#pgo-gen-}"': "JSON perf profile builder must validate derived PGO-use presets",
        },
        "scripts/json_perf_run_conditions.sh": {
            'validate_profile_presets "$src" "$profile"': "JSON perf run conditions must validate derived profile presets",
            'cmake-preset-build-dir.py" "$src" "$(release_preset "$profile")"': "JSON perf run conditions must validate release presets",
            'cmake-preset-build-dir.py" "$src" "$(pgo_use_preset "$profile")"': "JSON perf run conditions must validate PGO-use presets",
        },
        "scripts/send_zc_threshold_evidence.sh": {
            'cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET"': "SEND_ZC threshold evidence must validate configured preset",
        },
        "scripts/send_zc_nic_evidence.sh": {
            'cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET"': "SEND_ZC NIC evidence must validate configured preset",
        },
        "scripts/storage_read_evidence.sh": {
            'cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET"': "storage-read evidence must validate configured preset",
        },
        "scripts/work_queue_contention_evidence.sh": {
            'cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET"': "work-queue evidence must validate configured preset",
        },
        "scripts/db_pipeline_live_evidence.sh": {
            'cmake-preset-build-dir.py "$REPO_ROOT" "$PRESET"': "DB pipeline evidence must validate configured preset",
        },
        "benchmarks/README.md": {
            'PERF_BUILD_DIR="$(python3 scripts/cmake-preset-build-dir.py "$PWD" perf-clang-libcxx)"': "benchmark README must derive perf build dir from CMake presets",
        },
        "benchmarks/notes/json_direct_struct_bottlenecks.md": {
            'PERF_BUILD_DIR="$(python3 scripts/cmake-preset-build-dir.py "$PWD" perf-clang-libcxx)"': "JSON direct-struct benchmark note must derive perf build dir from CMake presets",
        },
        "docs/migration/examples.md": {
            'cmake-preset-build-dir.py "$PWD" <preset>': "examples migration doc must derive build dir from CMake presets",
        },
        "docs/build-ci-lanes.md": {
            'cmake-preset-build-dir.py "$PWD" fuzz-clang-stdcxx': "build CI lane doc must derive fuzz test dir from CMake presets",
            'cmake-preset-build-dir.py "$PWD" pgo-gen-clang-libcxx': "build CI lane doc must derive Clang PGO test dir from CMake presets",
            'cmake-preset-build-dir.py "$PWD" pgo-gen-gcc16-stdcxx': "build CI lane doc must derive GCC PGO test dir from CMake presets",
        },
        "docs/performance-hot-path-proposal.md": {
            'BASE_BUILD_DIR="$(python3 "$BASE_SRC/scripts/cmake-preset-build-dir.py" "$BASE_SRC" "$PRESET")"': "performance hot-path proposal must derive baseline build dir from CMake presets",
            'CAND_BUILD_DIR="$(python3 "$CAND_SRC/scripts/cmake-preset-build-dir.py" "$CAND_SRC" "$PRESET")"': "performance hot-path proposal must derive candidate build dir from CMake presets",
        },
    }
    forbidden = {
        "scripts/check-changed-cxx.sh": {
            'PRESET_ROOT="/tmp/': "changed-C++ hygiene script must not hardcode /tmp preset roots",
        },
        "scripts/run-sanitizer-matrix.sh": {
            "/tmp/$(basename": "matrix scripts must not reconstruct preset build dirs",
            "PRESET_ROOT": "matrix scripts must not reconstruct preset roots",
            "supported_profiles=": "matrix scripts must not keep generic profile allowlists",
        },
        "scripts/run-perf-matrix.sh": {
            "/tmp/$(basename": "matrix scripts must not reconstruct preset build dirs",
            "PRESET_ROOT": "matrix scripts must not reconstruct preset roots",
            "supported_profiles=": "matrix scripts must not keep generic profile allowlists",
        },
        "scripts/run-ctest.sh": {
            "supported_profiles=": "run-ctest must not keep a hand-maintained supported profile list",
        },
        "build-all.sh": {
            'PRESET_ROOT="$(dirname "${PRESET_ROOT}")"': "build-all must not reconstruct preset build dirs from dirname",
        },
        "scripts/perf_patch_sweep.sh": {
            "printf '/tmp/gcc-16/%s": "perf patch sweep must not hardcode /tmp/gcc-16 build dirs",
        },
        "scripts/stage-release-artifacts.sh": {
            'build_dir="/tmp/$(basename "$root")/$preset"': "release artifact staging must not hardcode /tmp/<repo>/<preset>",
        },
        "scripts/bench_record.sh": {
            'PROJECT_TMP="/tmp/$(basename "$REPO_ROOT")"': "benchmark recorder debug cleanup must not hardcode /tmp/<repo>",
        },
        "benchmarks/README.md": {
            "/tmp/<repo>/perf-clang-libcxx": "benchmark README must not hardcode perf preset build dirs",
        },
        "benchmarks/notes/json_direct_struct_bottlenecks.md": {
            "/tmp/<repo>/perf-clang-libcxx": "JSON direct-struct benchmark note must not hardcode perf preset build dirs",
        },
        "docs/migration/examples.md": {
            "/tmp/conflux/<preset>": "examples migration doc must not reconstruct preset build dirs",
        },
        "docs/build-ci-lanes.md": {
            "ctest --test-dir /tmp/conflux/fuzz-clang-stdcxx": "build CI lane doc must not hardcode fuzz preset build dir",
            "ctest --test-dir /tmp/conflux/pgo-gen-clang-libcxx": "build CI lane doc must not hardcode Clang PGO preset build dir",
            "ctest --test-dir /tmp/conflux/pgo-gen-gcc16-stdcxx": "build CI lane doc must not hardcode GCC PGO preset build dir",
        },
        "docs/performance-hot-path-proposal.md": {
            "/tmp/conflux-base/$PRESET": "performance hot-path proposal must not hardcode baseline preset build dir",
            "/tmp/conflux-cand/$PRESET": "performance hot-path proposal must not hardcode candidate preset build dir",
        },
    }
    errors: list[str] = []
    for path, markers in checks.items():
        text = read(path)
        errors.extend(message for marker, message in markers.items() if marker not in text)
    for path, markers in forbidden.items():
        text = read(path)
        errors.extend(message for marker, message in markers.items() if marker in text)
    if errors:
        fail("\n".join(sorted(errors)))


def check_cmake_extraction_contracts() -> None:
    checks = {
        "CMakeLists.txt": {
            "include(ConfluxProviderSelection)": "missing provider selection CMake module include",
            "include(ConfluxPython)": "missing Python configuration CMake module include",
            "include(ConfluxUringProbes)": "missing io_uring probe CMake module include",
            "include(ConfluxCompilerProbes)": "missing compiler probes CMake module include",
            "include(ConfluxOptionsTarget)": "missing options target CMake module include",
            "include(ConfluxCompilerWorkarounds)": "missing compiler workaround CMake module include",
            "include(ConfluxModuleLibrary)": "missing module-library helper CMake module include",
            "include(ConfluxComponentValidation)": "missing component validation CMake module include",
        },
        "cmake/ConfluxProviderSelection.cmake": {
            'set(CONFLUX_JSON_HASH_PROVIDER "${CONFLUX_EFFECTIVE_JSON_HASH_PROVIDER}")': "provider selection module must bridge effective provider requests",
        },
        "cmake/ConfluxPython.cmake": {
            "set(Python3_FIND_IMPLEMENTATIONS CPython)": "Python configuration module must prefer CPython",
        },
        "cmake/ConfluxUringProbes.cmake": {
            "function(conflux_configure_uring_probes target)": "io_uring probe module must define conflux_configure_uring_probes",
        },
        "cmake/ConfluxCompilerProbes.cmake": {
            "CONFLUX_HAS_WARNING_CLEAN_AUTO_UNDERSCORE_DISCARD": "compiler probes module must detect warning-clean underscore discard",
        },
        "cmake/ConfluxSimd.cmake": {
            "function(conflux_apply_aesni_source_options source)": "AES-NI source options must live with SIMD/ISA helpers",
        },
        "cmake/ConfluxOptionsTarget.cmake": {
            "add_library(conflux_options INTERFACE)": "options target module must define conflux_options",
        },
        "cmake/ConfluxModuleLibrary.cmake": {
            "function(conflux_add_module_library target)": "module-library helper must define conflux_add_module_library",
        },
        "cmake/ConfluxHeaderInterface.cmake": {
            "include(ConfluxComponentValidation)": "header interface must run component validation before defining header targets",
            "function(conflux_header_support_component target export_name)": "header support component metadata must use the shared header helper",
            "conflux_header_support_component(conflux_headers headers)": "headers support component must use the shared header helper",
            "include(ConfluxHeaderInstall)": "header interface must delegate header install helpers to ConfluxHeaderInstall.cmake",
            "function(conflux_validate_header_impl_metadata)": "header implementation metadata lists must be validated before package metadata assembly",
            "header implementation metadata lists are out of sync": "header implementation metadata validation must reject out-of-sync lists",
            "header implementation target '${_target}' is listed more than once": "header implementation metadata validation must reject duplicate targets",
            "header implementation component '${_component}' is listed more than once": "header implementation metadata validation must reject duplicate components",
            "header implementation component": "header implementation metadata validation must reject mismatched component/target pairs",
            "must use the header_impl_ package namespace": "header implementation metadata validation must enforce generated impl component namespace",
            "must pair with target 'conflux_${_component}'": "header implementation metadata validation must pair impl components with matching targets",
        },
        "cmake/ConfluxHeaderInstall.cmake": {
            "CONFLUX_HEADER_INSTALLED_GENERATED_HEADERS": "header install helper must track generated public headers selected for install",
            "function(conflux_collect_generated_detail_includes out)": "header install helper must collect generated detail includes from installed public headers",
            "function(conflux_install_generated_detail_headers)": "header install helper must install only referenced generated detail headers",
            "function(conflux_install_registered_public_headers)": "header install helper must own registered public header installation",
        },
        "cmake/ConfluxComponentValidation.cmake": {
            "function(conflux_require_component_flag request_var dependency_var diagnostic)": "component validation module must centralize simple component prerequisite checks",
            "CONFLUX_HTTP_ROUTER_STACK_REQUESTED": "component validation module must derive HTTP stack request flags",
            "CONFLUX_BUILD_FILE_IO_SYNC": "component validation module must validate file_io_sync requirements",
        },
        "cmake/ConfluxOptions.cmake": {
            "MODULE_INTERFACE preview support is limited to the checked": "module-interface configure must fail early on unsupported preview toolchains",
            "GCC 15, GCC 16, and Clang 21": "module-interface toolchain guard must name the preview evidence lanes",
        },
        "cmake/ConfluxCompilerWorkarounds.cmake": {
            'set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-O0")': "template compiler workaround must stay source-file scoped",
            "GCC 15 currently ICEs": "compiler workaround must keep its GCC ICE motivation",
            "GNU release builds have needed this fallback for the HTTP send": "HTTP server compiler workaround must keep its GNU release-build motivation",
        },
        "cmake/components/CryptoTargets.cmake": {
            'conflux_apply_aesni_source_options("${CONFLUX_SRC_ROOT}/crypto_aesni.cxx")': "crypto AES-NI source options must stay source-file scoped through the SIMD helper",
        },
        "cmake/components/TemplateTargets.cmake": {
            'conflux_apply_template_compiler_workarounds("${CONFLUX_SRC_ROOT}/template_impl.cxx")': "template compiler workaround must stay source-file scoped",
        },
        "cmake/components/RuntimeTargets.cmake": {
            "conflux_configure_uring_probes(conflux_uring)": "runtime CMake must configure io_uring probes at the conflux_uring target",
        },
        "cmake/components/HttpServerTargets.cmake": {
            'conflux_apply_http_server_compiler_workarounds("${CONFLUX_SRC_ROOT}/net/http_server_send.cxx")': "HTTP server compiler workaround must stay source-file scoped",
        },
        "cmake/components/HttpUmbrellaTargets.cmake": {
            "PRIVATE conflux_options": "HTTP umbrella target must inherit generated include paths and build macros from conflux_options",
        },
        "tests/HttpFacadeTests.cmake": {
            'set_source_files_properties(http_facade_test.cxx PROPERTIES COMPILE_OPTIONS "-fno-lto")': "HTTP facade GCC LTO fallback must stay scoped to the test source file",
        },
        "benchmarks/CMakeLists.txt": {
            "_conflux_bench_std26_disable_lto": "std::simd benchmark LTO fallback must be compiler-scoped",
            "_conflux_bench_json_std26_disable_lto": "JSON std::simd benchmark LTO fallback must be compiler-scoped",
        },
    }
    forbidden = {
        "CMakeLists.txt": {
            "check_cxx_source_runs(": "io_uring runtime probes must live in ConfluxUringProbes.cmake",
            'set_source_files_properties("${CONFLUX_SRC_ROOT}/crypto_aesni.cxx': "crypto AES-NI source options must not live directly in root CMake",
            "add_library(conflux_options INTERFACE)": "conflux_options target definition must live in ConfluxOptionsTarget.cmake",
            "function(conflux_add_module_library target)": "module-library helper must live in ConfluxModuleLibrary.cmake",
            "set(CONFLUX_HTTP_ROUTER_STACK_REQUESTED": "HTTP stack request derivation must live in ConfluxComponentValidation.cmake",
            'set_source_files_properties("${CONFLUX_SRC_ROOT}/template_impl.cxx': "template compiler workaround must live in ConfluxCompilerWorkarounds.cmake",
            'set_source_files_properties("${CONFLUX_SRC_ROOT}/net/http_server_send.cxx': "HTTP server compiler workaround must live in ConfluxCompilerWorkarounds.cmake",
        },
        "tests/CMakeLists.txt": {
            "set_target_properties(conflux_http_facade_tests PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)": "HTTP facade GCC LTO fallback must not disable IPO for the whole test target",
        },
        "scripts/check-optimized-presets.sh": {
            "subprocess": "optimized preset guard must not shell out once per preset",
        },
        "scripts/check-provider-policy-matrix.sh": {
            "CMAKE_DISABLE_FIND_PACKAGE_PkgConfig": "provider policy matrix must not pass warning-prone CMake PkgConfig disable flags",
        },
    }
    errors: list[str] = []
    for path, markers in checks.items():
        text = read(path)
        errors.extend(message for marker, message in markers.items() if marker not in text)
    for path, patterns in forbidden.items():
        text = read(path)
        errors.extend(message for marker, message in patterns.items() if marker in text)
    if errors:
        fail("\n".join(sorted(errors)))


def perf_patch_sweep_default_targets() -> list[str]:
    body = read("scripts/perf_patch_sweep.sh")
    try:
        function_body = body.split("targets_for_patch() {", 1)[1].split("benches_for_patch() {", 1)[0]
    except IndexError:
        fail("missing perf patch target defaults")
    targets: list[str] = []
    for line in function_body.splitlines():
        line = line.strip()
        if not line.startswith("printf '%s\\n' "):
            continue
        targets.extend(re.findall(r"\bconflux_[A-Za-z0-9_]+\b", line))
    return targets


def literal_build_targets_from_script(path: str) -> list[str]:
    text = read(path)
    targets: list[str] = []
    for line in text.splitlines():
        if "cmake --build" not in line or "--target" not in line:
            continue
        tail = line.split("--target", 1)[1]
        for token in re.findall(r"\bconflux_[A-Za-z0-9_]+\b", tail):
            targets.append(token)
    return targets


def shell_case_mapping(text: str, function_name: str) -> dict[str, str]:
    try:
        body = text.split(f"{function_name}() {{", 1)[1].split("\n}", 1)[0]
    except IndexError:
        fail(f"missing shell function: {function_name}")
    mapping: dict[str, str] = {}
    for match in re.finditer(
        r"^\s*([A-Za-z0-9_]+)\)\s+printf '%s\\n'\s+([A-Za-z0-9_]+)\s+;;",
        body,
        re.MULTILINE,
    ):
        mapping[match.group(1)] = match.group(2)
    if not mapping:
        fail(f"missing shell case mapping in {function_name}")
    return mapping


def check_script_default_benchmark_targets() -> None:
    declared = declared_cmake_targets([
        "CMakeLists.txt",
        "benchmarks/CMakeLists.txt",
        "tests/CMakeLists.txt",
        "tests/ApiSurfaceCompileFailTests.cmake",
        "tests/ApiSurfaceImportTests.cmake",
        "tests/CoreTests.cmake",
        "tests/DbTests.cmake",
        "tests/DnsSurfaceTests.cmake",
        "tests/E2ETests.cmake",
        "tests/ExternalTests.cmake",
        "tests/HttpCompressionTests.cmake",
        "tests/HttpAuthTests.cmake",
        "tests/HttpCoreTests.cmake",
        "tests/HttpFacadeTests.cmake",
        "tests/HttpJsonTests.cmake",
        "tests/HttpLifecycleTests.cmake",
        "tests/HttpObservabilityTests.cmake",
        "tests/HttpParseHelpersTests.cmake",
        "tests/HttpPolicyTests.cmake",
        "tests/HttpProxyTests.cmake",
        "tests/HttpResponseTests.cmake",
        "tests/HttpRouterDispatchTests.cmake",
        "tests/HttpRouterMatchTests.cmake",
        "tests/HttpServerHelpersTests.cmake",
        "tests/HttpServerTests.cmake",
        "tests/HttpStaticCoreTests.cmake",
        "tests/HttpVhostTests.cmake",
        "tests/IoTests.cmake",
        "tests/JsonTests.cmake",
        "tests/MainTests.cmake",
        "tests/SocketTests.cmake",
        "tests/TemplateProcessTests.cmake",
        "tests/WorkTests.cmake",
        "cmake/ConfluxInterfaceMode.cmake",
    ])
    default_sets = {
        "scripts/json_perf_build_profiles.sh": shell_default_array_values(
            read("scripts/json_perf_build_profiles.sh"),
            "targets",
            "JSON_PERF_TARGETS",
        ),
        "scripts/json_perf_run_conditions.sh": shell_default_array_values(
            read("scripts/json_perf_run_conditions.sh"),
            "targets",
            "JSON_PERF_TARGETS",
        ),
        "scripts/run-perf-matrix.sh": [shell_string_assignment(
            read("scripts/run-perf-matrix.sh"),
            "TARGET",
        )],
        "scripts/bench_record.sh": (
            literal_build_targets_from_script("scripts/bench_record.sh")
        ),
        "scripts/perf_patch_sweep.sh": perf_patch_sweep_default_targets(),
        "scripts/db_pipeline_live_evidence.sh": (
            literal_build_targets_from_script("scripts/db_pipeline_live_evidence.sh")
        ),
        "scripts/work_queue_contention_evidence.sh": (
            literal_build_targets_from_script("scripts/work_queue_contention_evidence.sh")
        ),
        "scripts/storage_read_evidence.sh": (
            literal_build_targets_from_script("scripts/storage_read_evidence.sh")
        ),
        "scripts/send_zc_nic_evidence.sh": (
            literal_build_targets_from_script("scripts/send_zc_nic_evidence.sh")
        ),
        "scripts/send_zc_threshold_evidence.sh": (
            literal_build_targets_from_script("scripts/send_zc_threshold_evidence.sh")
        ),
    }

    errors: list[str] = []
    for path, targets in default_sets.items():
        if not targets:
            errors.append(f"{path}: default benchmark target list must be non-empty")
            continue
        duplicates = sorted(target for target, count in Counter(targets).items() if count > 1)
        if duplicates:
            errors.append(f"{path}: duplicate default benchmark targets: {';'.join(duplicates)}")
        missing = sorted(target for target in targets if target not in declared)
        if missing:
            errors.append(f"{path}: default benchmark targets are not declared by CMake: {';'.join(missing)}")
    if errors:
        fail("\n".join(errors))


def check_json_perf_benchmark_maps() -> None:
    build_profiles = read("scripts/json_perf_build_profiles.sh")
    run_conditions = read("scripts/json_perf_run_conditions.sh")
    target_to_bench = shell_case_mapping(build_profiles, "bench_name_for_target")
    bench_to_target = shell_case_mapping(run_conditions, "bench_bin_name")
    errors: list[str] = []

    inverted = {bench: target for target, bench in target_to_bench.items()}
    append_set_delta_errors(
        errors,
        set(inverted),
        set(bench_to_target),
        "JSON perf benches missing from run-condition binary map: ",
        "JSON perf benches missing from build-profile target map: ",
    )
    for bench in sorted(set(inverted) & set(bench_to_target)):
        if bench_to_target[bench] != inverted[bench]:
            errors.append(
                f"JSON perf benchmark map mismatch for {bench}: "
                f"{inverted[bench]} != {bench_to_target[bench]}",
            )

    default_targets = shell_default_array_values(
        build_profiles,
        "targets",
        "JSON_PERF_TARGETS",
    )
    default_benches = shell_default_array_values(
        run_conditions,
        "benches",
        "JSON_PERF_BENCHES",
    )
    expected_default_benches = [target_to_bench[target] for target in default_targets]
    if default_benches != expected_default_benches:
        errors.append(
            "JSON perf default benches must match JSON perf default targets: "
            + ";".join(default_benches)
            + " != "
            + ";".join(expected_default_benches),
        )
    if errors:
        fail("\n".join(errors))


def package_smoke_wrapper_default_components() -> dict[str, str]:
    checks = {
        "scripts/check-package-smoke-core-isolated.sh": r"--components\s+core\b",
        "scripts/check-package-smoke-liburing-free.sh": r"--components\s+'([^']+)'",
        "scripts/check-package-smoke-runtime.sh": r"--components\s+'([^']+)'",
        "scripts/check-package-smoke-db.sh": r"--components\s+'([^']+)'",
        "scripts/check-package-smoke-mixed-module-header.sh": (
            r'components="\$\{CONFLUX_PACKAGE_SMOKE_MIXED_COMPONENTS:-([^}]+)\}"'
        ),
        "scripts/check-public-module-import-smoke.sh": (
            r'components="\$\{CONFLUX_PUBLIC_MODULE_IMPORT_SMOKE_COMPONENTS:-([^}]+)\}"'
        ),
    }
    defaults: dict[str, str] = {}
    for path, pattern in checks.items():
        match = re.search(pattern, read(path))
        if match is None:
            fail(f"missing wrapper default package smoke components in {path}")
        defaults[path] = match.group(1) if match.groups() else "core"
    return defaults


def check_package_smoke_wrapper_default_components() -> None:
    public_components = public_component_exports_from_registry()
    expected_defaults = {
        "scripts/check-package-smoke-core-isolated.sh": "core",
        "scripts/check-package-smoke-liburing-free.sh": "core;json;file_io_sync",
        "scripts/check-package-smoke-runtime.sh": "core;json;http;file_io_sync;work",
        "scripts/check-package-smoke-db.sh": "core;json;pg",
        "scripts/check-package-smoke-mixed-module-header.sh": "core;json;http",
        "scripts/check-public-module-import-smoke.sh": "core;json;http",
    }
    errors: list[str] = []
    defaults = package_smoke_wrapper_default_components()
    if set(defaults) != set(expected_defaults):
        append_set_delta_errors(
            errors,
            set(expected_defaults),
            set(defaults),
            "package smoke wrapper defaults missing scripts: ",
            "package smoke wrapper defaults contain unexpected scripts: ",
        )
    for path, components in defaults.items():
        requested = [component for component in components.split(";") if component]
        if not requested:
            errors.append(f"{path}: wrapper default package smoke components must be non-empty")
            continue
        expected = [component for component in expected_defaults.get(path, "").split(";") if component]
        if expected and requested != expected:
            errors.append(
                f"{path}: wrapper default package smoke components changed: "
                + ";".join(requested)
                + " != "
                + ";".join(expected),
            )
        unknown = sorted(component for component in requested if component not in public_components)
        if unknown:
            errors.append(
                f"{path}: wrapper default package smoke components must be public registry exports: "
                + ";".join(unknown),
            )
    if errors:
        fail("\n".join(errors))


def check_package_smoke_wrapper_contracts() -> None:
    checks = {
        "scripts/check-package-smoke-core-isolated.sh": {
            "compress_backend_zlib_like.hxx": "core-isolated package smoke must reject unrelated generated compression detail headers",
        },
        "scripts/check-package-smoke-mixed-module-header.sh": {
            'CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-1}"': "mixed module/header package smoke must cap its default build concurrency",
        },
    }
    errors: list[str] = []
    for path, markers in checks.items():
        text = read(path)
        errors.extend(message for marker, message in markers.items() if marker not in text)
    core_cmake_definitions = shell_cmake_definitions(
        read("scripts/check-package-smoke-core-isolated.sh"),
    )
    if core_cmake_definitions.get("CONFLUX_JSON_HASH_PROVIDER") != "XXHASH":
        errors.append("core-isolated package smoke must force the external JSON hash provider")
    liburing_free_forbidden = shell_semicolon_flag_value(
        read("scripts/check-package-smoke-liburing-free.sh"),
        "--forbid-external-deps",
    )
    for token in ["LIBURING", "LIBPQ", "OPENSSL"]:
        if token not in liburing_free_forbidden:
            errors.append(
                f"liburing-free package smoke must explicitly forbid {token}",
            )
    runtime_pkg_config_probes = shell_pkg_config_exists_probes(
        read("scripts/check-package-smoke-runtime.sh"),
    )
    if "liburing" not in runtime_pkg_config_probes:
        errors.append("runtime package smoke must gate on real liburing")
    db_pkg_config_probes = shell_pkg_config_exists_probes(
        read("scripts/check-package-smoke-db.sh"),
    )
    for package in ["libpq", "liburing"]:
        if package not in db_pkg_config_probes:
            errors.append(f"DB package smoke must gate on {package}")
    if not shell_flag_present(read("scripts/check-package-smoke-db.sh"), "--enable-db-smoke"):
        errors.append("DB package smoke must enable DB component checks")
    core_isolated = read("scripts/check-package-smoke-core-isolated.sh")
    core_forbidden = shell_semicolon_flag_value(core_isolated, "--forbid-components")
    expected_core_forbidden = {
        "curated",
        "extended",
        "complete",
        "json",
        "http",
        "http1",
        "http2",
        "http3",
        "http_protocol",
        "template",
        "pg",
        "db",
    }
    append_set_delta_errors(
        errors,
        expected_core_forbidden,
        core_forbidden,
        "core-isolated package smoke forbidden components missing: ",
        "core-isolated package smoke forbidden components unexpected: ",
    )
    if "--forbid-external-deps" in core_isolated:
        errors.append("core-isolated package smoke must rely on the default core external-dependency policy")
    if errors:
        fail("\n".join(sorted(errors)))


def check_package_smoke_project_contract() -> None:
    paths = [
        Path("cmake/package-smoke/CMakeLists.txt"),
        *sorted(Path("cmake/package-smoke").glob("*.cmake")),
    ]
    text = "\n".join(read(str(path)) for path in paths)
    required_markers = {
        "find_package(conflux REQUIRED COMPONENTS": "package smoke project must consume find_package(conflux COMPONENTS ...)",
        "CONFLUX_PACKAGE_SMOKE_COMPONENTS must name at least one component": "package smoke project must reject empty component lists",
        "CONFLUX_PACKAGE_SMOKE_COMPONENTS must request public components": "package smoke project must reject support components in requested component lists",
        "add_executable(conflux_package_smoke \"${_conflux_package_smoke_source}\")": "package smoke project must compile a generated downstream executable",
        "target_link_libraries(conflux_package_smoke PRIVATE": "package smoke project must link installed namespaced targets",
        "add_test(NAME package-smoke/run": "package smoke project must run the downstream executable",
        "CONFLUX_PACKAGE_SMOKE_EXERCISE_LINKED_APIS": "package smoke must distinguish declaration-only and linked API lanes",
        "conflux::json::parse": "package smoke must exercise installed JSON implementation symbols when available",
        "conflux::build_info_summary": "package smoke must exercise installed core implementation symbols when available",
        "read_text_file_nothrow": "package smoke must exercise installed file_io_sync implementation symbols when available",
        "CONFLUX_HAS_JSON": "package smoke must assert installed JSON feature macros",
        "set(CMAKE_CXX_SCAN_FOR_MODULES OFF)": "header package smoke must disable module scanning by default",
        "set(CMAKE_CXX_SCAN_FOR_MODULES ON)": "module package smoke must enable module scanning",
        "import conflux.types;": "module package smoke source must import an installed conflux module",
        "#include <conflux/types.hpp>": "header package smoke source must include only the installed core header",
        "available_components=${conflux_AVAILABLE_COMPONENTS}": "package smoke summary must report available components",
        "visible_components=${conflux_VISIBLE_COMPONENTS}": "package smoke summary must report visible components",
        "visible_support_targets=${conflux_VISIBLE_SUPPORT_TARGETS}": "package smoke summary must report visible support targets",
        "resolved_external_deps=${conflux_RESOLVED_EXTERNAL_DEPS}": "package smoke summary must report resolved external deps",
        "CONFLUX_PACKAGE_SMOKE_FORBIDDEN_EXTERNAL_DEPS": "package smoke must support negative external dependency assertions",
        "CONFLUX_PACKAGE_SMOKE_FAST_COMPILE": "package smoke must expose a fast-compile option",
        "CONFLUX_PACKAGE_SMOKE_ENABLE_IMPORT_STD": "package smoke must make import-std experimental support opt-in",
        "CONFLUX_PACKAGE_SMOKE_API_SURFACE": "package smoke must expose an expected API-surface assertion",
        "CONFLUX_API_SURFACE_LEVEL != CONFLUX_API_SURFACE_": "package smoke must compile-check installed API-surface macros",
        "expected_api_surface=${CONFLUX_PACKAGE_SMOKE_API_SURFACE}": "package smoke summary must report expected API surface",
        "conflux_apply_package_smoke_build_policy": "package smoke targets must apply fast-compile policy",
        "conflux_link_package_smoke_base_targets": "package smoke targets must centralize common base target links",
        "CXX_SCAN_FOR_MODULES OFF": "header package smoke must disable module scanning for header targets",
        "found unrequested visible target": "package smoke must reject unrequested visible targets",
        "runtime_requires_liburing=${CONFLUX_RUNTIME_REQUIRES_LIBURING}": "package smoke summary must report runtime/liburing status",
        'include("${CMAKE_CURRENT_LIST_DIR}/PublicModuleImports.cmake")': "package smoke project must include the public-module import fragment",
        'include("${CMAKE_CURRENT_LIST_DIR}/MixedModuleHeader.cmake")': "package smoke project must include the mixed module/header fragment",
        'include("${CMAKE_CURRENT_LIST_DIR}/ComponentSmokes.cmake")': "package smoke project must include the component smoke fragment",
    }
    missing = sorted(message for marker, message in required_markers.items() if marker not in text)
    if missing:
        fail("\n".join(missing))
    component_smokes = read("cmake/package-smoke/ComponentSmokes.cmake")
    if "import conflux.features;" in component_smokes:
        fail("narrow component package smokes must not import the aggregate conflux.features module")


def check_package_smoke_runner_contract() -> None:
    text = read("scripts/run-package-config-smoke.sh")
    required_markers = {
        "--forbid-external-deps": "package smoke runner must expose negative external dependency assertions",
        "--api-surface": "package smoke runner must expose API-surface assertions",
        "--enable-import-std": "package smoke runner must expose import-std opt-in",
        "cmake --build \"$build_dir\"": "package smoke runner must build the downstream project",
        "ctest --test-dir \"$build_dir\" --output-on-failure": "package smoke runner must run downstream CTest",
        "conflux-package-smoke-summary.txt": "package smoke runner must print the downstream summary",
        "normalize_cmake_list": "package smoke runner must normalize semicolon list arguments",
        "--components must not be empty": "package smoke runner must reject empty component lists",
        "--components must request public components": "package smoke runner must reject support components in requested component lists",
        "header_impl_*": "package smoke runner must reject generated header implementation support components",
        "forbid_all_external_deps=": "package smoke runner must centralize default forbidden external dependency tokens",
        "forbid_runtime_db_template_components=": "package smoke runner must centralize default forbidden runtime/db/template component policy",
        "--enable-db": "package smoke runner must expose a DB-enabled smoke option",
    }
    missing = sorted(message for marker, message in required_markers.items() if marker not in text)
    if missing:
        fail("\n".join(missing))


def check_install_tree_smoke_runner_contract() -> None:
    text = read("scripts/run-install-tree-smoke.sh")
    required_markers = {
        "cmake --build \"$build_dir\" --target install": "install-tree smoke runner must build and install conflux",
        "--interface-mode": "install-tree smoke runner must forward interface mode",
        "--api-surface": "install-tree smoke runner must forward API surface",
        "--enable-import-std-smoke": "install-tree smoke runner must forward import-std smoke opt-in",
        "--generator": "install-tree smoke runner must forward generator",
        "extra_cmake_args": "install-tree smoke runner must forward extra configure args",
        "run-package-config-smoke.sh": "install-tree smoke runner must consume the installed prefix",
        "--components must request public components": "install-tree smoke runner must reject support components in requested component lists",
        "header_impl_*": "install-tree smoke runner must reject generated header implementation support components",
        "--enable-db-smoke": "install-tree smoke runner must forward DB-enabled package smoke",
        "--forbid-components": "install-tree smoke runner must forward forbidden component assertions",
        "--forbid-external-deps": "install-tree smoke runner must forward forbidden external dependency assertions",
    }
    missing = sorted(message for marker, message in required_markers.items() if marker not in text)
    if missing:
        fail("\n".join(missing))


def check_install_tree_ctest_helpers() -> None:
    options = read("cmake/ConfluxOptions.cmake")
    tests = read("tests/CMakeLists.txt")
    header = read("cmake/ConfluxHeaderInterface.cmake")
    required_markers = {
        "function(conflux_add_package_config_install_tree_test source_dir build_dir)": "package-config install-tree CTest must use the shared filtered argument helper",
        "function(conflux_add_install_tree_smoke_test source_dir)": "install-tree smoke CTest must use the shared filtered argument helper",
        "_conflux_install_tree_smoke_args": "install-tree smoke helper must build its command from a filtered argument list",
        "function(conflux_escape_package_smoke_components out_var)": "package smoke CTest helpers must share component-list escaping",
        'string(REPLACE ";" "\\\\;" _conflux_escaped_components': "package smoke CTest helpers must preserve component lists as one command argument",
    }
    missing = sorted(message for marker, message in required_markers.items() if marker not in options)
    if 'conflux_add_install_tree_smoke_test("${CMAKE_SOURCE_DIR}")' not in tests:
        missing.append("install-tree smoke CTest must use the shared filtered argument helper")
    if 'conflux_add_install_tree_smoke_test("${CMAKE_CURRENT_SOURCE_DIR}")' not in header:
        missing.append("header install-tree smoke CTest must use the shared filtered argument helper")
    if "conflux_add_package_config_install_tree_test(" not in tests:
        missing.append("package-config install-tree CTest must use the shared filtered argument helper")
    if "conflux_add_package_config_install_tree_test(" not in header:
        missing.append("header package-config install-tree CTest must use the shared filtered argument helper")
    if "mixed-module-header-smoke>" in tests or "mixed-module-header-smoke>" in header:
        missing.append("install-tree smoke CTest must not use generator expressions that emit empty arguments")
    if missing:
        fail("\n".join(missing))


def check_provider_selection_order() -> None:
    check_marker_order(
        read("CMakeLists.txt"),
        [
            "conflux_apply_preset()",
            "include(ConfluxProviderSelection)",
            "include(Dependencies)",
        ],
        "provider selection must run after presets and before dependency discovery",
    )


def check_python_setup_order() -> None:
    check_marker_order(
        read("CMakeLists.txt"),
        [
            "include(ConfluxPython)",
            "include(ConfluxInterfaceMode)",
            "conflux_configure_interface_mode()",
        ],
        "Python setup must run before interface-mode configuration",
    )


def check_compiler_probe_order() -> None:
    check_marker_order(
        read("CMakeLists.txt"),
        [
            "include(ConfluxCompilerProbes)",
            "include(ConfluxOptionsTarget)",
        ],
        "compiler probes must run before conflux_options publishes probe results",
    )


def check_options_target_order() -> None:
    check_marker_order(
        read("CMakeLists.txt"),
        [
            "include(ConfluxOptionsTarget)",
            "include(cmake/components/CoreTargets.cmake)",
        ],
        "conflux_options target setup must run before component targets",
    )


def check_duplicate_ctest_names() -> None:
    names: dict[str, str] = {}
    duplicates: list[str] = []
    pattern = re.compile(r"add_test\s*\(\s*NAME\s+([^\s\)]+)", re.MULTILINE)
    for path in cmake_test_cmake_paths():
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        for match in pattern.finditer(text):
            name = match.group(1).strip("'\"")
            if "$" in name:
                continue
            line_no = text.count("\n", 0, match.start()) + 1
            location = f"{path}:{line_no}"
            if name in names:
                duplicates.append(f"{name}: {names[name]} and {location}")
            else:
                names[name] = location
    if duplicates:
        fail("duplicate CTest names:\n" + "\n".join(duplicates))


def external_tokens_from_metadata() -> set[str]:
    registry = read("cmake/ConfluxExternalDependencyRegistry.cmake")
    match = re.search(
        r"set\(CONFLUX_EXTERNAL_DEPENDENCY_TOKENS(?P<body>.*?)\)",
        registry,
        re.DOTALL,
    )
    if match is None:
        fail("missing CONFLUX_EXTERNAL_DEPENDENCY_TOKENS")
    return set(re.findall(r"\b[A-Z][A-Z0-9_]+\b", match.group("body")))


def component_declarations_from_registry() -> list[tuple[str, str, str]]:
    registry = read("cmake/ConfluxComponentRegistry.cmake")
    declarations = re.findall(r'"([^"|]+)\|([^"|]+)\|(REQUESTABLE|SUPPORT)"', registry)
    if not declarations:
        fail("missing CONFLUX_COMPONENT_DECLARATIONS")
    return declarations


def component_exports_from_registry() -> set[str]:
    return {export for _, export, _ in component_declarations_from_registry()}


def component_exports_from_registry_kind(kind: str) -> set[str]:
    return {
        export
        for _, export, declaration_kind in component_declarations_from_registry()
        if declaration_kind == kind
    }


def check_component_registry_contract() -> None:
    registry = read("cmake/ConfluxComponentRegistry.cmake")
    required_markers = {
        "set(CONFLUX_COMPONENT_DECLARATIONS": "component registry must keep a single authoritative declaration list",
        "list(APPEND CONFLUX_PUBLIC_COMPONENT_DECLARATIONS": "public component declarations must be derived from the authoritative registry",
        "list(APPEND CONFLUX_SUPPORT_COMPONENT_DECLARATIONS": "support component declarations must be derived from the authoritative registry",
        'REQUESTABLE")': "component registry must name requestable components by kind",
        'SUPPORT")': "component registry must name support components by kind",
    }
    errors = sorted(message for marker, message in required_markers.items() if marker not in registry)
    declarations = component_declarations_from_registry()
    exports = [export for _, export, _ in declarations]
    duplicate_exports = sorted(name for name, count in Counter(exports).items() if count > 1)
    if duplicate_exports:
        errors.append("component registry duplicate exports: " + ";".join(duplicate_exports))
    if errors:
        fail("\n".join(errors))


def public_component_exports_from_registry() -> set[str]:
    return component_exports_from_registry_kind("REQUESTABLE")


def support_component_exports_from_registry() -> set[str]:
    return component_exports_from_registry_kind("SUPPORT")


def generated_header_support_exports() -> set[str]:
    return {"headers", "header_impl"}


def check_external_dependency_tokens() -> None:
    config = read("cmake/conflux-config.cmake.in")
    metadata = read("cmake/ConfluxGeneratePackageMetadata.cmake.in")
    registry = read("cmake/ConfluxExternalDependencyRegistry.cmake")
    metadata_tokens = external_tokens_from_metadata()
    errors: list[str] = []
    required_registry_prefixes = [
        "CONFLUX_EXTERNAL_DEPENDENCY_TARGETS_",
        "CONFLUX_EXTERNAL_DEPENDENCY_KIND_",
        "CONFLUX_EXTERNAL_DEPENDENCY_PACKAGES_",
    ]
    for token in metadata_tokens:
        for prefix in required_registry_prefixes:
            if f"set({prefix}{token} " not in registry:
                errors.append(f"external dependency registry missing {prefix}{token}")
    config_markers = {
        "ConfluxExternalDependencyRegistry.cmake": "package config must include the external dependency registry",
        "CONFLUX_EXTERNAL_DEPENDENCY_TOKENS": "package config must validate external dependency tokens through the registry",
        "CONFLUX_EXTERNAL_DEPENDENCY_KIND_${token}": "package config must resolve external dependency kind through the registry",
        "CONFLUX_EXTERNAL_DEPENDENCY_PACKAGES_${token}": "package config must resolve external dependency packages through the registry",
    }
    errors.extend(message for marker, message in config_markers.items() if marker not in config)
    metadata_markers = {
        "ConfluxExternalDependencyRegistry.cmake": "package metadata generator must include the external dependency registry",
        "CONFLUX_EXTERNAL_DEPENDENCY_TOKENS": "package metadata generator must map external targets through the registry",
        "CONFLUX_EXTERNAL_DEPENDENCY_TARGETS_${_candidate}": "package metadata generator must map external targets through registry target lists",
    }
    errors.extend(message for marker, message in metadata_markers.items() if marker not in metadata)
    if re.search(r'token STREQUAL "[A-Z0-9_]+"', config):
        errors.append("package config must not carry a separate external-token resolver ladder")
    if re.search(r"set\(_token [A-Z0-9_]+\)", metadata):
        errors.append("package metadata generator must not carry a separate external-token mapping ladder")
    install_text = read("cmake/ConfluxInstall.cmake")
    header_text = read("cmake/ConfluxHeaderInterface.cmake")
    for path, text in [
        ("cmake/ConfluxInstall.cmake", install_text),
        ("cmake/ConfluxHeaderInterface.cmake", header_text),
    ]:
        if "ConfluxExternalDependencyRegistry.cmake" not in text:
            errors.append(f"{path} must install the external dependency registry")
    if errors:
        fail("\n".join(errors))


def check_package_config_uses_generated_component_metadata() -> None:
    config = read("cmake/conflux-config.cmake.in")
    required_markers = {
        "conflux-component-targets.cmake": "package config must include generated component target metadata",
        "conflux-component-deps.cmake": "package config must include generated component dependency metadata",
        "conflux-component-external-deps.cmake": "package config must include generated external dependency metadata",
        "confluxTargets-${_export_component}.cmake": "package config must include requested split exported targets",
        "set(conflux_AVAILABLE_COMPONENTS": "package config must expose available components",
        "set(conflux_AVAILABLE_SUPPORT_TARGETS": "package config must expose available support targets",
        "set(conflux_VISIBLE_COMPONENTS": "package config must expose visible components",
        "set(conflux_VISIBLE_SUPPORT_TARGETS": "package config must expose visible support targets",
        "set(conflux_RESOLVED_EXTERNAL_DEPS": "package config must expose resolved external deps",
        "_conflux_import_component": "package config must compute requested component dependency closure",
        "_conflux_find_external_dep": "package config must resolve closure-scoped external deps",
    }
    missing = sorted(message for marker, message in required_markers.items() if marker not in config)
    if missing:
        fail("\n".join(missing))
    if re.search(r"^set\(_conflux_component_deps_", config, re.MULTILINE):
        fail("package config must not contain a hand-written component dependency table")
    if re.search(r"^set\(_conflux_component_order", config, re.MULTILINE):
        fail("package config must not contain a hand-written component import order")
    if re.search(r"@CONFLUX_INSTALL_NEEDS_.*pkg_check_modules|if\(@CONFLUX_INSTALL_NEEDS_", config):
        fail("package config must not resolve optional deps from install-wide booleans")


def check_install_and_dependency_contracts() -> None:
    install = read("cmake/ConfluxInstall.cmake")
    header = read("cmake/ConfluxHeaderInterface.cmake")
    interface_mode = read("cmake/ConfluxInterfaceMode.cmake")
    required_install_markers = {
        "configure_package_config_file(": "missing configure_package_config_file()",
        "write_basic_package_version_file(": "missing write_basic_package_version_file()",
        "VERSION ${PROJECT_VERSION}": "package version file must use PROJECT_VERSION",
        "install(EXPORT confluxTargets": "missing install(EXPORT confluxTargets)",
        'install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/ConfluxGeneratePackageMetadata.cmake")': "missing install-time package metadata generator",
        "NAMESPACE conflux::": "export namespace must stay conflux::",
    }
    errors = sorted(
        message
        for marker, message in required_install_markers.items()
        if marker not in install
    )
    profile_helper_markers = {
        "function(conflux_register_header_public_profile_hpps)": "profile header hpp registration must use a shared helper",
        "if(NOT umbrella IN_LIST CONFLUX_PACKAGE_COMPONENTS)": "profile hpp wrappers must require the aggregate package component",
        "foreach(_profile IN ITEMS curated extended complete)": "profile hpp helper must cover all aggregate profiles",
    }
    errors.extend(
        message for marker, message in profile_helper_markers.items()
        if marker not in interface_mode
    )
    for path, text in [
        ("cmake/ConfluxInstall.cmake", install),
        ("cmake/ConfluxHeaderInterface.cmake", header),
    ]:
        if "conflux_register_header_public_profile_hpps()" not in text:
            errors.append(f"{path} must use shared profile hpp registration")
        for profile in ["curated", "extended", "complete"]:
            if f"conflux_register_header_public_hpp({profile})" in text:
                errors.append(f"{path} must not register {profile}.hpp unconditionally")

    dependencies = read("cmake/Dependencies.cmake")
    match = re.search(
        r"FetchContent_Declare\(\s*JSONTestSuite(?P<body>.*?)\)",
        dependencies,
        re.DOTALL,
    )
    if match is None:
        errors.append("JSONTestSuite fetch must be declared explicitly")
    else:
        body = match.group("body")
        if "GIT_REPOSITORY https://github.com/nst/JSONTestSuite.git" not in body:
            errors.append("JSONTestSuite fetch must name the upstream repository explicitly")
        tag = re.search(r"\bGIT_TAG\s+([^\s\)]+)", body)
        if tag is None:
            errors.append("JSONTestSuite fetch must pin a full commit SHA")
        elif tag.group(1) in {"master", "main"}:
            errors.append("JSONTestSuite fetch must not use a floating branch")
        elif not re.fullmatch(r"[0-9a-f]{40}", tag.group(1)):
            errors.append("JSONTestSuite fetch must pin a full commit SHA")
        shallow = re.search(r"\bGIT_SHALLOW\s+([^\s\)]+)", body)
        if shallow is None or shallow.group(1) != "FALSE":
            errors.append("JSONTestSuite full SHA fetch must not be shallow")
    if errors:
        fail("\n".join(errors))


def check_build_docs_guard_contracts() -> None:
    checks = {
        "tests/CMakeLists.txt": {
            'include("${CMAKE_CURRENT_LIST_DIR}/TestHelpers.cmake")': "tests CMake must include the shared test helper fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/ApiSurfaceCompileFailTests.cmake")': "tests CMake must include the API surface compile-fail fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/ApiSurfaceImportTests.cmake")': "tests CMake must include the API surface import smoke fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/MainTests.cmake")': "tests CMake must include the main test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/CoreTests.cmake")': "tests CMake must include the core test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/ExternalTests.cmake")': "tests CMake must include the external test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpCompressionTests.cmake")': "tests CMake must include the HTTP compression test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpAuthTests.cmake")': "tests CMake must include the HTTP auth test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpCoreTests.cmake")': "tests CMake must include the HTTP core test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpFacadeTests.cmake")': "tests CMake must include the HTTP facade test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpJsonTests.cmake")': "tests CMake must include the HTTP JSON test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpObservabilityTests.cmake")': "tests CMake must include the HTTP observability test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpResponseTests.cmake")': "tests CMake must include the HTTP response test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpServerHelpersTests.cmake")': "tests CMake must include the HTTP server helpers test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpParseHelpersTests.cmake")': "tests CMake must include the HTTP parse helpers test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpPolicyTests.cmake")': "tests CMake must include the HTTP policy test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpProxyTests.cmake")': "tests CMake must include the HTTP proxy test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpStaticCoreTests.cmake")': "tests CMake must include the HTTP static core test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpRouterDispatchTests.cmake")': "tests CMake must include the HTTP router dispatch test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpRouterMatchTests.cmake")': "tests CMake must include the HTTP router match test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpVhostTests.cmake")': "tests CMake must include the HTTP vhost test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpServerTests.cmake")': "tests CMake must include the HTTP server test fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/HttpLifecycleTests.cmake")': "tests CMake must include the HTTP lifecycle test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/TemplateProcessTests.cmake")': "tests CMake must include the template/process test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/JsonTests.cmake")': "tests CMake must include the JSON test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/SocketTests.cmake")': "tests CMake must include the socket/runtime test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/WorkTests.cmake")': "tests CMake must include the work/runtime test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/NetworkSurfaceTests.cmake")': "tests CMake must include the network surface compile-fail fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/DnsSurfaceTests.cmake")': "tests CMake must include the DNS surface compile-fail fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/IoTests.cmake")': "tests CMake must include the io/dns test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/DbTests.cmake")': "tests CMake must include the DB test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/E2ETests.cmake")': "tests CMake must include the e2e test target fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/TestDiscovery.cmake")': "tests CMake must include the test discovery registration fragment",
            'include("${CMAKE_CURRENT_LIST_DIR}/BuildAndDocsChecks.cmake")': "tests CMake must include the build/docs CTest registration fragment",
        },
        "tests/BuildAndDocsChecks.cmake": {
            "function(conflux_add_python_check test_name script_path labels)": "build/docs CTest fragment must centralize simple Python checks",
            "conflux_add_python_check(build/cmake-source-files": "missing CMake source-file CTest guard",
            "conflux_add_python_check(build/component-map": "missing component-map CTest guard",
            "conflux_add_python_check(build/http-facade-snapshot": "missing HTTP facade snapshot CTest guard",
            "add_test(NAME build/package-config": "missing package-config CTest guard",
            "add_test(NAME build/module-fragility-regression": "module-fragility CTest guard must live in build/docs fragment",
            "add_test(NAME build/optimized-presets": "optimized-presets CTest guard must live in build/docs fragment",
            "conflux_add_python_check(build/cmake-preset-build-dir": "CMake preset build-dir helper CTest guard must live in build/docs fragment",
            "add_test(NAME build/header-first-contact-smoke": "missing default first-contact header smoke CTest guard",
            "CONFLUX_RUN_HEADER_COMPONENT_SMOKE": "full header component smoke must be opt-in",
            "add_test(NAME build/header-component-smoke": "missing full header component smoke CTest guard",
            "conflux_add_python_check(docs/planning-state": "missing planning-state CTest guard",
            "conflux_add_python_check(docs/release-docs": "missing release-docs CTest guard",
            "conflux_add_python_check(docs/package-docs": "missing package-docs CTest guard",
            "conflux_add_python_check(docs/release-notes": "missing release-notes CTest guard",
        },
        "tests/TestHelpers.cmake": {
            "function(conflux_catch_extra_args out_var)": "test helper fragment must centralize Catch discovery arguments",
            "function(conflux_add_compile_fail_test)": "test helper fragment must centralize compile-fail target registration",
            "add_library(${CF_TARGET} EXCLUDE_FROM_ALL OBJECT ${CF_SOURCE})": "compile-fail checks must stay compile-time OBJECT target checks",
            "scripts/check-compile-fail-target.sh": "compile-fail checks must use the compile-fail target runner",
            "function(conflux_discover_db_integration_tests target)": "test helper fragment must centralize DB integration discovery",
        },
        "tests/ApiSurfaceImportTests.cmake": {
            "add_executable(conflux_api_surface_core_import_smoke": "API surface import fragment must define core import smoke",
            "conflux_api_surface_${_surface}_import_smoke": "API surface import fragment must define aggregate import smokes",
            "api-surface/import-${_surface}": "API surface import fragment must register aggregate import CTests",
        },
        "tests/ApiSurfaceCompileFailTests.cmake": {
            "conflux_api_surface_curated_compile_fail_workpool": "API surface compile-fail fragment must keep curated surface checks",
            "conflux_api_surface_extended_compile_fail_iouring": "API surface compile-fail fragment must keep extended surface checks",
            "conflux_crypto_compile_fail_global_base64_encode": "API surface compile-fail fragment must keep crypto checks",
            "conflux_utils_compile_fail_global_url_decode": "API surface compile-fail fragment must keep utils checks",
            "conflux_process_compile_fail_global_spawn": "API surface compile-fail fragment must keep process checks",
            "conflux_template_compile_fail_global_environment": "API surface compile-fail fragment must keep template checks",
            "conflux_file_io_compile_fail_global_pipe_pool": "API surface compile-fail fragment must keep file-io checks",
            "conflux_work_compile_fail_global_work_pool": "API surface compile-fail fragment must keep work checks",
            "conflux_types_compile_fail_global_io_error": "API surface compile-fail fragment must keep type checks",
        },
        "tests/MainTests.cmake": {
            "add_executable(conflux_tests)": "main test fragment must define the main test target",
            "http_e2e_middleware.cxx": "main test fragment must keep middleware e2e source wiring",
            "target_include_directories(conflux_tests PRIVATE \"${CMAKE_SOURCE_DIR}/src/net\")": "main test fragment must keep net-private include path",
            "target_link_libraries(conflux_tests PRIVATE ZLIB::ZLIB)": "main test fragment must keep optional ZLIB edge",
        },
        "tests/CoreTests.cmake": {
            "add_executable(conflux_crypto_tests crypto_test.cxx)": "core test fragment must define crypto tests",
            "add_executable(conflux_utils_tests utils_test.cxx)": "core test fragment must define utils tests",
            "add_executable(conflux_password_hash_tests password_hash_test.cxx)": "core test fragment must define password hash tests",
            "add_executable(conflux_config_tests config_test.cxx)": "core test fragment must define config tests when available",
        },
        "tests/ExternalTests.cmake": {
            "add_executable(conflux_tls_external)": "external test fragment must define TLS external tests when available",
            "add_executable(conflux_smtp_tests smtp_test.cxx)": "external test fragment must define SMTP tests when available",
            "add_executable(conflux_h2_external)": "external test fragment must define HTTP/2 external tests when available",
            "add_executable(conflux_third_party_conformance_server third_party_conformance_server.cxx)": "external test fragment must define third-party conformance server when enabled",
        },
        "tests/TemplateProcessTests.cmake": {
            "add_executable(conflux_template_tests template_test.cxx)": "template/process test fragment must define template tests",
            "add_executable(conflux_process_tests process_test.cxx)": "template/process test fragment must define process tests",
        },
        "tests/HttpCompressionTests.cmake": {
            "conflux_http_compression_compile_fail_global_compress_options": "HTTP compression test fragment must keep compress options compile-fail check",
            "conflux_http_compression_compile_fail_global_gzip_backend": "HTTP compression test fragment must keep gzip backend compile-fail check",
            "conflux_http_compression_compile_fail_global_compress_middleware": "HTTP compression test fragment must keep compression middleware compile-fail check",
        },
        "tests/HttpAuthTests.cmake": {
            "conflux_http_auth_compile_fail_global_basic_auth_options": "HTTP auth test fragment must keep basic auth compile-fail check",
            "conflux_http_auth_compile_fail_global_auth_throttle_middleware": "HTTP auth test fragment must keep throttle middleware compile-fail check",
            "conflux_http_auth_compile_fail_global_jwt_middleware": "HTTP auth test fragment must keep JWT middleware compile-fail check",
            "conflux_http_auth_compile_fail_global_password_hash": "HTTP auth test fragment must keep password hash compile-fail check",
            "conflux_http_auth_compile_fail_global_cookie_signing_options_from_config": "HTTP auth test fragment must keep cookie signing config compile-fail check",
            "conflux_http_auth_compile_fail_global_csrf_middleware": "HTTP auth test fragment must keep CSRF middleware compile-fail check",
        },
        "tests/HttpCoreTests.cmake": {
            "add_executable(http_request_assert_probe http_request_assert_probe.cxx)": "HTTP core test fragment must define request assert probe",
            "add_executable(conflux_http_core_tests)": "HTTP core test fragment must define HTTP core tests",
            "conflux_http_core_compile_fail_global_request": "HTTP core test fragment must keep request compile-fail check",
            "conflux_http_core_compile_fail_global_http_fields_view": "HTTP core test fragment must keep fields view compile-fail check",
        },
        "tests/HttpFacadeTests.cmake": {
            "add_executable(conflux_http_facade_tests http_facade_test.cxx)": "HTTP facade test fragment must define facade tests",
            "add_executable(conflux_http_facade_import_smoke http_facade_import_smoke.cxx)": "HTTP facade test fragment must define import smoke",
            "add_library(conflux_http_facade_api_snapshot OBJECT http_facade_api_snapshot.cxx)": "HTTP facade test fragment must define API snapshot",
            "conflux_http_facade_compile_fail_raw_string": "HTTP facade test fragment must keep raw string compile-fail check",
            "conflux_http_facade_compile_fail_global_config": "HTTP facade test fragment must keep config compile-fail check",
            "conflux_http_facade_compile_fail_router_alias": "HTTP facade test fragment must keep router alias compile-fail check",
            "conflux_http_router_compile_fail_global_router": "HTTP facade test fragment must keep router compile-fail checks",
            "conflux_http_router_static_compile_fail_global_static_route_handler": "HTTP facade test fragment must keep router static compile-fail checks",
            "conflux_http_static_compile_fail_global_static_options": "HTTP facade test fragment must keep static options compile-fail check",
        },
        "tests/HttpJsonTests.cmake": {
            "add_executable(conflux_http_json_tests http_json_test.cxx)": "HTTP JSON test fragment must define JSON tests",
            "conflux_http_json_compile_fail_provider_template_response_alias": "HTTP JSON test fragment must keep provider template response alias compile-fail check",
        },
        "tests/HttpLifecycleTests.cmake": {
            "add_executable(conflux_send_zc_lifecycle_tests send_zc_lifecycle_test.cxx)": "HTTP lifecycle test fragment must define send-zc lifecycle tests when enabled",
        },
        "tests/HttpObservabilityTests.cmake": {
            "conflux_http_middleware_compile_fail_global_request_id_options": "HTTP observability test fragment must keep request id compile-fail check",
            "conflux_http_middleware_compile_fail_global_tracing_middleware": "HTTP observability test fragment must keep tracing middleware compile-fail check",
            "conflux_http_metrics_compile_fail_global_counter": "HTTP observability test fragment must keep counter compile-fail check",
            "conflux_http_metrics_compile_fail_global_pressure_formatter": "HTTP observability test fragment must keep pressure formatter compile-fail check",
        },
        "tests/HttpResponseTests.cmake": {
            "add_executable(conflux_http_response_tests http_response_test.cxx)": "HTTP response test fragment must define response tests",
            "conflux_http_response_compile_fail_global_deferred_response": "HTTP response test fragment must keep deferred response compile-fail check",
            "conflux_http_response_compile_fail_global_response": "HTTP response test fragment must keep response compile-fail check",
        },
        "tests/HttpServerHelpersTests.cmake": {
            "add_executable(conflux_http_server_helpers_tests http_server_helpers_test.cxx)": "HTTP server helpers test fragment must define server helpers tests",
            "conflux_http_server_helpers_compile_fail_global_format_response": "HTTP server helpers test fragment must keep format response compile-fail check",
            "conflux_http_server_helpers_compile_fail_global_parse_cookies": "HTTP server helpers test fragment must keep parse cookies compile-fail check",
        },
        "tests/HttpParseHelpersTests.cmake": {
            "conflux_http_parse_helpers_compile_fail_global_chunk_state": "HTTP parse helpers test fragment must keep chunk state compile-fail check",
            "conflux_http_parse_helpers_compile_fail_global_parse_urlencoded": "HTTP parse helpers test fragment must keep parse urlencoded compile-fail check",
            "conflux_http_parse_helpers_compile_fail_global_content_type": "HTTP parse helpers test fragment must keep content type compile-fail check",
        },
        "tests/HttpPolicyTests.cmake": {
            "conflux_http_policy_compile_fail_global_cache_control_options": "HTTP policy test fragment must keep cache control compile-fail check",
            "conflux_http_policy_compile_fail_global_trailing_slash_options": "HTTP policy test fragment must keep trailing slash compile-fail check",
            "conflux_http_middleware_compile_fail_global_cors_options": "HTTP policy test fragment must keep CORS compile-fail check",
            "conflux_http_middleware_compile_fail_global_etag_middleware": "HTTP policy test fragment must keep ETag middleware compile-fail check",
        },
        "tests/HttpProxyTests.cmake": {
            "conflux_http_proxy_compile_fail_global_proxy_options": "HTTP proxy test fragment must keep proxy options compile-fail check",
            "conflux_http_proxy_compile_fail_global_blocking_proxy": "HTTP proxy test fragment must keep blocking proxy compile-fail check",
            "conflux_http_proxy_compile_fail_global_async_proxy": "HTTP proxy test fragment must keep async proxy compile-fail check",
        },
        "tests/HttpStaticCoreTests.cmake": {
            "conflux_http_static_core_compile_fail_global_static_request": "HTTP static core test fragment must keep static request compile-fail check",
            "conflux_http_static_core_compile_fail_global_static_cache_store": "HTTP static core test fragment must keep static cache store compile-fail check",
            "conflux_http_static_core_compile_fail_global_normalize_static_path": "HTTP static core test fragment must keep normalize static path compile-fail check",
        },
        "tests/HttpRouterDispatchTests.cmake": {
            "conflux_http_router_dispatch_compile_fail_global_deferred_task_options": "HTTP router dispatch test fragment must keep deferred task options compile-fail check",
            "conflux_http_router_dispatch_compile_fail_global_dispatch_sync_routes": "HTTP router dispatch test fragment must keep dispatch sync routes compile-fail check",
            "conflux_http_router_dispatch_compile_fail_global_router_run_async_http_task": "HTTP router dispatch test fragment must keep router async task compile-fail check",
        },
        "tests/HttpRouterMatchTests.cmake": {
            "conflux_http_router_match_compile_fail_global_segment": "HTTP router match test fragment must keep segment compile-fail check",
            "conflux_http_router_match_compile_fail_global_parse_pattern": "HTTP router match test fragment must keep parse pattern compile-fail check",
        },
        "tests/HttpVhostTests.cmake": {
            "conflux_http_vhost_compile_fail_global_vhost_router": "HTTP vhost test fragment must keep vhost router compile-fail check",
        },
        "tests/HttpServerTests.cmake": {
            "conflux_http_server_compile_fail_global_http_server": "HTTP server test fragment must keep HTTP server compile-fail check",
        },
        "tests/TestDiscovery.cmake": {
            "include(CTest)": "test discovery fragment must enable CTest",
            "conflux_discover_tests(conflux_json_conformance_external)": "test discovery fragment must register JSON conformance tests",
            "conflux_discover_tests(conflux_json_testsuite_gate)": "test discovery fragment must register JSONTestSuite gate when available",
            "conflux_discover_tests(conflux_recv_bundle_tests)": "test discovery fragment must register recv bundle tests when available",
            "conflux_discover_tests(conflux_tcp_listener_tests)": "test discovery fragment must register TCP listener tests",
            "conflux_discover_tests(conflux_direct_slot_pool_tests)": "test discovery fragment must register direct slot pool tests",
            "conflux_discover_stress_tests(conflux_http_full_drain_contract_e2e)": "test discovery fragment must register full-drain stress tests",
            "conflux_discover_db_integration_tests(conflux_db_integration)": "test discovery fragment must register DB integration discovery",
        },
        "tests/JsonTests.cmake": {
            "add_executable(conflux_json_tests json_test.cxx)": "JSON test fragment must define the main JSON test target",
            "add_executable(conflux_json_conformance_external json_conformance_external.cxx)": "JSON test fragment must define external conformance tests",
            "add_executable(conflux_json_testsuite_gate json_testsuite_gate.cxx)": "JSON test fragment must define JSONTestSuite gate target when available",
        },
        "tests/SocketTests.cmake": {
            "add_executable(conflux_resource_tests resource_limits_test.cxx)": "socket test fragment must define resource tests",
            "add_executable(conflux_recv_bundle_tests)": "socket test fragment must define recv bundle tests when enabled",
            "add_executable(conflux_tcp_listener_tests tcp_listener_test.cxx)": "socket test fragment must define TCP listener tests",
            "add_executable(conflux_recv_bundle_e2e_tests)": "socket test fragment must define recv bundle e2e tests when enabled",
        },
        "tests/WorkTests.cmake": {
            "add_library(conflux_work_api_snapshot OBJECT work_api_snapshot.cxx)": "work test fragment must define the work API snapshot",
            "add_executable(conflux_work_tests work_test.cxx)": "work test fragment must define the main work tests",
            "add_executable(conflux_direct_slot_pool_tests direct_slot_pool_test.cxx)": "work test fragment must define direct slot pool tests",
            "conflux_direct_slot_pool_compile_fail_global_pool": "work test fragment must keep direct slot pool compile-fail checks",
        },
        "tests/NetworkSurfaceTests.cmake": {
            "conflux_net_cancel_compile_fail_global_active_task_cancel_relay": "network surface fragment must keep net cancel compile-fail check",
            "conflux_net_io_buffer_compile_fail_global_io_buffer": "network surface fragment must keep io buffer compile-fail check",
            "conflux_http2_compile_fail_global_configure_alpn": "network surface fragment must keep HTTP/2 compile-fail check",
            "conflux_http3_compile_fail_global_listener": "network surface fragment must keep HTTP/3 compile-fail check",
        },
        "tests/DnsSurfaceTests.cmake": {
            "conflux_dns_compile_fail_global_address_family": "DNS surface fragment must keep address family compile-fail check",
            "conflux_dns_compile_fail_global_message": "DNS surface fragment must keep message compile-fail check",
            "conflux_dns_compile_fail_global_current_resolver_scope": "DNS surface fragment must keep current resolver scope compile-fail check",
        },
        "tests/IoTests.cmake": {
            "add_executable(conflux_cq_overflow_tests cq_overflow_test.cxx)": "io test fragment must define CQ overflow tests",
            "add_executable(conflux_file_io_tests file_io_test.cxx)": "io test fragment must define file io tests",
            "add_executable(conflux_dns_codec_tests dns_codec_test.cxx)": "io test fragment must define DNS codec tests",
            "add_executable(conflux_socket_task_ring_tests socket_task_ring_test.cxx)": "io test fragment must define socket task ring tests",
        },
        "tests/DbTests.cmake": {
            "conflux_db_compile_fail_legacy_module": "DB test fragment must keep legacy module compile-fail check",
            "conflux_db_compile_fail_global_result": "DB test fragment must keep result compile-fail check",
            "add_executable(conflux_db_tests db_test.cxx)": "DB test fragment must define DB tests",
            "add_executable(conflux_db_integration db_integration_test.cxx)": "DB test fragment must define DB integration tests",
        },
        "tests/E2ETests.cmake": {
            "add_executable(conflux_client_cancellation_e2e client_cancellation_e2e.cxx)": "e2e test fragment must define client cancellation tests",
            "add_executable(conflux_file_io_http_e2e)": "e2e test fragment must define file io HTTP tests",
            "add_executable(conflux_http_full_drain_contract_e2e)": "e2e test fragment must define full-drain contract tests",
            "add_executable(conflux_http_overflow_stress_tests)": "e2e test fragment must define overflow stress tests",
        },
        "scripts/check-cmake-source-files.py": {
            'ROOT / "tests"': "CMake source-file guard must scan test CMake fragments",
            'ROOT / "tests" / "CMakeLists.txt"': "CMake source-file guard must scan tests CMakeLists",
            'ROOT / "cmake" / "package-smoke" / "CMakeLists.txt"': "CMake source-file guard must scan package-smoke CMakeLists",
            'ROOT / "benchmarks" / "CMakeLists.txt"': "CMake source-file guard must scan benchmarks CMakeLists",
            'ROOT / "examples" / "CMakeLists.txt"': "CMake source-file guard must scan examples CMakeLists",
            'ROOT / "fuzz" / "CMakeLists.txt"': "CMake source-file guard must scan fuzz CMakeLists",
            'EXTENSION_PATTERN = "|".join': "CMake source-file guard must derive its regex from SOURCE_EXTENSIONS",
        },
        "scripts/check-component-map.py": {
            "declared as both public and support": "component-map guard must reject public/support component ownership overlap",
            "uses an unsafe export name": "component-map guard must reject unsafe component export names",
            "must use support-component naming": "component-map guard must reject support components without support naming",
        },
        "scripts/cmake-preset-build-dir.py": {
            "duplicate configure preset": "CMake preset build-dir helper must reject duplicate configure preset names",
            "cyclic preset include involving": "CMake preset build-dir helper must reject cyclic preset includes",
            "cyclic preset inheritance involving": "CMake preset build-dir helper must reject cyclic preset inheritance",
        },
        "scripts/check-cmake-preset-build-dir.py": {
            "cyclic preset includes must be rejected": "CMake preset build-dir helper guard must cover cyclic include rejection",
            "cyclic preset inheritance must be rejected": "CMake preset build-dir helper guard must cover cyclic inheritance rejection",
        },
        "scripts/check-optimized-presets.sh": {
            "def inherited_field": "optimized preset guard must resolve inherited fields in-process",
            "def expanded_binary_dir": "optimized preset guard must derive binary dirs without per-preset subprocesses",
            "actual = expanded_binary_dir(name)": "optimized preset guard must validate binary dirs in-process",
        },
    }
    errors: list[str] = []
    for path, markers in checks.items():
        text = read(path)
        errors.extend(message for marker, message in markers.items() if marker not in text)
    if errors:
        fail("\n".join(sorted(errors)))


def check_package_metadata_generator_contract() -> None:
    metadata = read("cmake/ConfluxGeneratePackageMetadata.cmake.in")
    required_markers = {
        "cmake_policy(PUSH)": "package metadata generator must scope CMake policy changes",
        "cmake_policy(SET CMP0057 NEW)": "package metadata generator must enable IN_LIST in install-script mode",
        "cmake_policy(POP)": "package metadata generator must restore CMake policy state",
        "function(_conflux_validate_parallel_lists components targets label)": "package metadata generator must validate component/target list pairing",
        "component/target lists differ in length": "package metadata generator must reject mismatched component/target lists",
        "is listed more than once": "package metadata generator must reject duplicate component/target list entries",
        "cannot be used in generated CMake variable names": "package metadata generator must reject unsafe generated CMake variable suffixes",
        "is not a valid conflux namespaced target": "package metadata generator must reject invalid namespaced target entries",
        "must pair with target": "package metadata generator must reject mismatched component/target pairs",
        "function(_conflux_validate_component_partitions)": "package metadata generator must validate component partitions",
        "all component list must match requestable plus support components": "package metadata generator must reject component partition drift",
        "all target list must match requestable plus support targets": "package metadata generator must reject target partition drift",
    }
    missing = sorted(message for marker, message in required_markers.items() if marker not in metadata)
    if missing:
        fail("\n".join(missing))
    header_install = read("cmake/ConfluxHeaderInstall.cmake")
    if 'install(DIRECTORY "${CONFLUX_GENERATED_INCLUDE_DIR}/conflux/detail/generated/"' in header_install:
        fail("header interface must not install the entire generated detail header directory")


def check_package_smoke_external_tokens() -> None:
    tokens = external_tokens_from_metadata()
    runner = read("scripts/run-package-config-smoke.sh")
    liburing_free = read("scripts/check-package-smoke-liburing-free.sh")
    scripts = "\n".join(
        runner if path == "scripts/run-package-config-smoke.sh" else read(path)
        for path in [
            "scripts/run-package-config-smoke.sh",
            "scripts/check-package-smoke-liburing-free.sh",
            "scripts/check-package-smoke-core-isolated.sh",
        ]
    )
    missing = sorted(token for token in tokens if token not in scripts)
    if missing:
        fail(f"external tokens missing from package smoke isolation scripts: {';'.join(missing)}")

    for variable, value in re.findall(r"^(forbid_[A-Za-z0-9_]*external_deps)=\"([^\"]*)\"", runner, re.MULTILINE):
        policy_tokens = {token for token in value.split(";") if token}
        unknown = sorted(policy_tokens - tokens)
        if unknown:
            fail(f"{variable} contains unknown external tokens: {';'.join(unknown)}")

    all_policy = shell_semicolon_list_var(runner, "forbid_all_external_deps")
    errors: list[str] = []
    append_set_delta_errors(
        errors,
        tokens,
        all_policy,
        "external tokens missing from package smoke all-forbidden policy: ",
        "unknown external tokens in package smoke all-forbidden policy: ",
    )

    core_isolated = read("scripts/check-package-smoke-core-isolated.sh")
    if "--forbid-external-deps" in core_isolated:
        errors.append("core-isolated package smoke must rely on the default core external-dependency policy")
    if "--components core" not in core_isolated:
        errors.append("core-isolated package smoke must request the core component")
    liburing_free_forbidden = shell_semicolon_flag_value(liburing_free, "--forbid-external-deps")
    append_set_delta_errors(
        errors,
        tokens - {"XXHASH"},
        liburing_free_forbidden,
        "external tokens missing from liburing-free forbidden list: ",
        "unknown external tokens in liburing-free forbidden list: ",
    )
    if errors:
        fail("\n".join(errors))


def check_core_isolated_forbidden_components() -> None:
    runner = read("scripts/run-package-config-smoke.sh")
    core_isolated = read("scripts/check-package-smoke-core-isolated.sh")
    liburing_free = read("scripts/check-package-smoke-liburing-free.sh")
    allowed_components = component_exports_from_registry() | {"db"}
    for variable, value in re.findall(r"^(forbid_[A-Za-z0-9_]*components)=\"([^\"]*)\"", runner, re.MULTILINE):
        policy_components = {component for component in value.split(";") if component}
        unknown = sorted(policy_components - allowed_components)
        if unknown:
            fail(f"{variable} contains unknown package components: {';'.join(unknown)}")

    runner_forbidden = shell_semicolon_list_var(runner, "forbid_runtime_db_template_components")
    if "--components core" not in core_isolated:
        fail("core-isolated package smoke must request the core component")
    errors: list[str] = []
    append_set_delta_errors(
        errors,
        {"http", "http1", "http2", "http3", "http_protocol", "template", "pg", "db"},
        runner_forbidden,
        "default core isolation policy is missing component entries: ",
        "default core isolation policy contains unexpected component entries: ",
    )
    append_set_delta_errors(
        errors,
        {
            "curated",
            "extended",
            "complete",
            "json",
            "http",
            "http1",
            "http2",
            "http3",
            "http_protocol",
            "template",
            "pg",
            "db",
        },
        shell_semicolon_flag_value(core_isolated, "--forbid-components"),
        "strict core-isolated smoke forbidden components missing entries: ",
        "strict core-isolated smoke forbidden components contain unexpected entries: ",
    )
    append_set_delta_errors(
        errors,
        {"http", "http1", "http2", "http3", "http_protocol", "work", "dns", "template", "db", "pg"},
        shell_semicolon_flag_value(liburing_free, "--forbid-components"),
        "liburing-free forbidden components missing isolation entries: ",
        "liburing-free forbidden components contain unexpected entries: ",
    )
    if errors:
        fail("\n".join(errors))


def check_package_smoke_policy_cases_use_variables() -> None:
    runner = read("scripts/run-package-config-smoke.sh")
    case_body = runner.split('if [[ "$components" != *";"* ]]; then', 1)[1].split("fi", 1)[0]
    inline_assignments = re.findall(
        r'^\s*(forbidden_(?:components|external_deps))="[^"$]*;[^"$]*\$\{',
        case_body,
        re.MULTILINE,
    )
    if inline_assignments:
        fail(
            "package smoke single-component cases must use named policy variables for: "
            + ";".join(sorted(set(inline_assignments))),
        )
    referenced_policies = set(re.findall(r"\$\{(forbid_[A-Za-z0-9_]+)\}", case_body))
    unused_policies = sorted(shell_policy_variables(runner) - referenced_policies)
    if unused_policies:
        fail("package smoke policy variables are not used in single-component cases: " + ";".join(unused_policies))


def check_duplicate_target_link_libraries() -> None:
    paths = [
        Path("CMakeLists.txt"),
        *sorted(Path("cmake").glob("*.cmake")),
        Path("cmake/package-smoke/CMakeLists.txt"),
        Path("benchmarks/CMakeLists.txt"),
        Path("examples/CMakeLists.txt"),
        Path("fuzz/CMakeLists.txt"),
        Path("tests/CMakeLists.txt"),
        *sorted(Path("tests").glob("*.cmake")),
    ]
    duplicates: list[str] = []
    for path in paths:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        edges: list[tuple[str, str, str]] = []
        for match in re.finditer(r"target_link_libraries\((.*?)\)", text, re.DOTALL):
            tokens = match.group(1).split()
            if not tokens:
                continue
            target = tokens[0]
            if "$" in target:
                continue
            scope = "PRIVATE"
            for token in tokens[1:]:
                if token in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                    scope = token
                elif token.startswith("$") or token.startswith("${"):
                    continue
                else:
                    edges.append((target, scope, token))

        duplicates.extend(
            f"{path}: {target} links {item} as {scope} {count} times"
            for (target, scope, item), count in sorted(Counter(edges).items())
            if count > 1
        )
    if duplicates:
        fail("\n".join(duplicates))


def check_installed_surface_aliases() -> None:
    options = read("cmake/ConfluxOptions.cmake")
    config = read("cmake/conflux-config.cmake.in")
    if "_conflux_surface_has_" in options:
        fail("build-tree surface macros must derive directly from conflux_set_api_surface_presence")
    build_macros = {
        suffix.upper()
        for suffix in re.findall(r"conflux_set_api_surface_presence\(([a-z0-9_]+)", options)
    }
    build_macros.add("FEATURES")
    installed_aliases = set(
        re.findall(r"_conflux_installed_surface_definitions ([A-Z0-9_]+) 1", config),
    )
    allowed_install_only = {"DB_COMPAT"}
    missing = sorted(installed_aliases - build_macros - allowed_install_only)
    if missing:
        fail(
            "installed surface aliases missing from build-tree surface macros: "
            + ";".join(missing),
        )


def check_metrics_status_is_graph_gated() -> None:
    provider = read("cmake/ConfluxProviderResolution.cmake")
    if "if(NOT (CONFLUX_WANT_HTTP_OBSERVABILITY OR CONFLUX_WANT_HTTP_SERVER))" not in provider:
        fail("metrics provider status must be gated by the active HTTP graph")
    if "HTTP observability not in feature set" in provider:
        fail("metrics provider status must stay quiet for feature graphs without HTTP observability")


def check_release_artifact_staging_contract() -> None:
    staging = read("scripts/stage-release-artifacts.sh")
    guard = read("scripts/check-release-artifact.py")
    required = {
        "check-release-artifact.py": "release artifact staging must self-check staged output",
        'cmake --preset "$preset"': "release artifact staging must prefer the preset build",
        "--feature-set": "release artifact staging must expose an explicit feature-set",
        'CONFLUX_FEATURE_SET="$feature_set"': "release artifact staging explicit build path must pass the selected feature-set",
        "module-header-bridge-manifest.json": "release artifact staging must include the bridge manifest",
    }
    missing = sorted(message for marker, message in required.items() if marker not in staging)
    if "python_version" not in guard:
        missing.append("release artifact guard must validate bridge python metadata")
    if missing:
        fail("\n".join(missing))


def main() -> int:
    if len(sys.argv) > 2:
        print("usage: check-package-config-structure.py [repo-root]", file=sys.stderr)
        return 2
    if len(sys.argv) == 2:
        Path(sys.argv[1]).resolve(strict=True)
        os.chdir(sys.argv[1])

    check_no_legacy_stdsimd_option()
    check_no_explicit_build_parallelism()
    check_provider_policy_scenarios_are_isolated()
    check_run_build_artifact_root_examples_are_declared()
    check_compile_time_bench_defaults()
    check_header_bridge_optional_inputs()
    check_header_http_impls_do_not_pull_json()
    check_header_impl_lists_have_no_duplicates()
    check_header_source_ids_exist()
    check_component_registry_contract()
    check_header_support_components_are_limited()
    check_header_public_components_use_registry_exports()
    check_header_interface_contracts()
    check_cmake_preset_names_unique()
    check_cmake_preset_references()
    check_test_preset_filters()
    check_install_smoke_presets()
    check_matrix_script_presets()
    check_build_all_presets()
    check_script_default_presets()
    check_preset_build_dir_usage_contracts()
    check_cmake_extraction_contracts()
    check_script_default_benchmark_targets()
    check_json_perf_benchmark_maps()
    check_provider_selection_order()
    check_python_setup_order()
    check_compiler_probe_order()
    check_options_target_order()
    check_duplicate_ctest_names()
    check_external_dependency_tokens()
    check_package_config_uses_generated_component_metadata()
    check_install_and_dependency_contracts()
    check_build_docs_guard_contracts()
    check_package_metadata_generator_contract()
    check_package_smoke_external_tokens()
    check_core_isolated_forbidden_components()
    check_package_smoke_wrapper_default_components()
    check_package_smoke_wrapper_contracts()
    check_package_smoke_project_contract()
    check_package_smoke_runner_contract()
    check_install_tree_smoke_runner_contract()
    check_install_tree_ctest_helpers()
    check_package_smoke_policy_cases_use_variables()
    check_duplicate_target_link_libraries()
    check_installed_surface_aliases()
    check_metrics_status_is_graph_gated()
    check_release_artifact_staging_contract()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
