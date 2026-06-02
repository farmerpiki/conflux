#!/usr/bin/env python3
"""Fail when docs/component-map.md drifts from public CMake components."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPONENT_SOURCE = ROOT / "cmake" / "ConfluxComponentRegistry.cmake"
COMPONENT_MAP = ROOT / "docs" / "component-map.md"

PUBLIC_BLOCK_RE = re.compile(
    r"set\(CONFLUX_PUBLIC_COMPONENT_DECLARATIONS(?P<body>.*?)\)",
    re.DOTALL,
)
SUPPORT_BLOCK_RE = re.compile(
    r"set\(CONFLUX_SUPPORT_COMPONENT_DECLARATIONS(?P<body>.*?)\)",
    re.DOTALL,
)
PUBLIC_COMPONENT_RE = re.compile(r'"([^"|]+)\|([^"|]+)"')
DOC_COMPONENT_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`conflux::([^`]+)`\s*\|",
    re.MULTILINE,
)
DOC_SUPPORT_COMPONENT_RE = re.compile(
    r"^\|\s*`(_[^`]+)`\s*\|\s*[^|]*\|\s*[^|]*\|",
    re.MULTILINE,
)


def declared_components(block_name: str, pattern: re.Pattern[str]) -> dict[str, str]:
    text = COMPONENT_SOURCE.read_text(encoding="utf-8")
    block = pattern.search(text)
    if block is None:
        raise ValueError(f"missing {block_name}")
    components: dict[str, str] = {}
    for target, export_name in PUBLIC_COMPONENT_RE.findall(block.group("body")):
        if export_name in components:
            raise ValueError(f"duplicate CMake component export name: {export_name}")
        components[export_name] = target
    return components


def public_components() -> dict[str, str]:
    return declared_components("CONFLUX_PUBLIC_COMPONENT_DECLARATIONS", PUBLIC_BLOCK_RE)


def support_components() -> dict[str, str]:
    return declared_components("CONFLUX_SUPPORT_COMPONENT_DECLARATIONS", SUPPORT_BLOCK_RE)


def documented_components() -> dict[str, str]:
    text = COMPONENT_MAP.read_text(encoding="utf-8")
    components: dict[str, str] = {}
    for component, target in DOC_COMPONENT_RE.findall(text):
        if component.startswith("_"):
            continue
        if component in components:
            raise ValueError(f"duplicate documented component: {component}")
        components[component] = target
    return components


def documented_support_components() -> set[str]:
    text = COMPONENT_MAP.read_text(encoding="utf-8")
    components: set[str] = set()
    for component in DOC_SUPPORT_COMPONENT_RE.findall(text):
        if component in components:
            raise ValueError(f"duplicate documented support component: {component}")
        components.add(component)
    return components


def main() -> int:
    failures: list[str] = []
    cmake_components = public_components()
    cmake_support_components = support_components()
    doc_components = documented_components()
    doc_support_components = documented_support_components()

    for component in sorted(cmake_components.keys() & cmake_support_components.keys()):
        failures.append(f"CMake component `{component}` is declared as both public and support")

    all_targets: dict[str, str] = {}
    for component, target in {**cmake_components, **cmake_support_components}.items():
        owner = all_targets.get(target)
        if owner is not None:
            failures.append(
                f"component registry target `{target}` is declared by both `{owner}` and `{component}`"
            )
        all_targets[target] = component

    for component, target in sorted(cmake_components.items()):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", component):
            failures.append(f"public component `{component}` uses an unsafe export name")
        if component.startswith("_"):
            failures.append(f"public component `{component}` must not use support-component naming")
        if target != "conflux" and not target.startswith("conflux_"):
            failures.append(f"public component `{component}` uses unexpected target `{target}`")

    for component, target in sorted(cmake_support_components.items()):
        if not re.fullmatch(r"_[A-Za-z][A-Za-z0-9_]*", component):
            failures.append(f"support component `{component}` uses an unsafe export name")
        if not component.startswith("_"):
            failures.append(f"support component `{component}` must use support-component naming")
        if not target.startswith("conflux_"):
            failures.append(f"support component `{component}` uses unexpected target `{target}`")

    for component in sorted(cmake_components.keys() - doc_components.keys()):
        failures.append(f"missing docs/component-map.md row for CMake component `{component}`")

    for component in sorted(doc_components.keys() - cmake_components.keys()):
        failures.append(f"docs/component-map.md documents non-CMake component `{component}`")

    for component in sorted(cmake_support_components.keys() - doc_support_components):
        failures.append(f"missing docs/component-map.md support row for CMake component `{component}`")

    for component in sorted(doc_support_components - cmake_support_components.keys()):
        failures.append(f"docs/component-map.md documents non-CMake support component `{component}`")

    for component in sorted(cmake_components.keys() & doc_components.keys()):
        documented_target = doc_components[component]
        if documented_target != component:
            failures.append(
                "component-map target mismatch for "
                f"`{component}`: expected `conflux::{component}`, found `conflux::{documented_target}`"
            )

    if failures:
        print("component-map guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("component-map: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"component-map guard failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
