#!/usr/bin/env python3
"""Fail when docs/component-map.md drifts from public CMake components."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPONENT_SOURCE = ROOT / "cmake" / "ConfluxComponentRegistry.cmake"
COMPONENT_MAP = ROOT / "docs" / "component-map.md"

COMPONENT_RE = re.compile(
    r'"([^"|]+)\|([^"|]+)\|(REQUESTABLE|SUPPORT)\|'
    r'(STABLE|ADVANCED|EXPERIMENTAL|INTERNAL_SUPPORT)"'
)
DOC_COMPONENT_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`conflux::([^`]+)`\s*\|",
    re.MULTILINE,
)
DOC_PUBLIC_ROW_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`conflux::([^`]+)`\s*\|\s*([^|]+?)\s*\|"
    r"\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|",
    re.MULTILINE,
)
DOC_SUPPORT_COMPONENT_RE = re.compile(
    r"^\|\s*`(_[^`]+)`\s*\|\s*[^|]*\|\s*[^|]*\|",
    re.MULTILINE,
)


def declared_components(kind: str) -> dict[str, tuple[str, str]]:
    text = COMPONENT_SOURCE.read_text(encoding="utf-8")
    components: dict[str, tuple[str, str]] = {}
    for target, export_name, declared_kind, tier in COMPONENT_RE.findall(text):
        if declared_kind != kind:
            continue
        if export_name in components:
            raise ValueError(f"duplicate CMake component export name: {export_name}")
        components[export_name] = (target, tier)
    if not components:
        raise ValueError(f"missing {kind} component declarations")
    return components


def public_components() -> dict[str, tuple[str, str]]:
    return declared_components("REQUESTABLE")


def support_components() -> dict[str, tuple[str, str]]:
    return declared_components("SUPPORT")


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


def documented_public_rows() -> dict[str, tuple[str, str, str, str]]:
    text = COMPONENT_MAP.read_text(encoding="utf-8")
    rows: dict[str, tuple[str, str, str, str]] = {}
    for component, target, imports, api_surface, contracts in DOC_PUBLIC_ROW_RE.findall(text):
        if component.startswith("_"):
            continue
        if component in rows:
            raise ValueError(f"duplicate documented component row: {component}")
        rows[component] = (
            target.strip(),
            imports.strip(),
            api_surface.strip(),
            contracts.strip(),
        )
    return rows


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
    doc_rows = documented_public_rows()
    doc_support_components = documented_support_components()

    for component in sorted(cmake_components.keys() & cmake_support_components.keys()):
        failures.append(f"CMake component `{component}` is declared as both public and support")

    all_targets: dict[str, str] = {}
    for component, (target, _) in {**cmake_components, **cmake_support_components}.items():
        owner = all_targets.get(target)
        if owner is not None:
            failures.append(
                f"component registry target `{target}` is declared by both `{owner}` and `{component}`"
            )
        all_targets[target] = component

    for component, (target, tier) in sorted(cmake_components.items()):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", component):
            failures.append(f"public component `{component}` uses an unsafe export name")
        if component.startswith("_"):
            failures.append(f"public component `{component}` must not use support-component naming")
        if target != "conflux" and not target.startswith("conflux_"):
            failures.append(f"public component `{component}` uses unexpected target `{target}`")
        if tier == "INTERNAL_SUPPORT":
            failures.append(f"public component `{component}` must not use internal-support tier")

    for component, (target, tier) in sorted(cmake_support_components.items()):
        if not re.fullmatch(r"_[A-Za-z][A-Za-z0-9_]*", component):
            failures.append(f"support component `{component}` uses an unsafe export name")
        if not component.startswith("_"):
            failures.append(f"support component `{component}` must use support-component naming")
        if not target.startswith("conflux_"):
            failures.append(f"support component `{component}` uses unexpected target `{target}`")
        if tier != "INTERNAL_SUPPORT":
            failures.append(f"support component `{component}` must use internal-support tier")

    for component in sorted(cmake_components.keys() - doc_components.keys()):
        failures.append(f"missing docs/component-map.md row for CMake component `{component}`")

    for component in sorted(doc_components.keys() - cmake_components.keys()):
        failures.append(f"docs/component-map.md documents non-CMake component `{component}`")

    for component in sorted(cmake_components.keys() - doc_rows.keys()):
        failures.append(f"missing docs/component-map.md public row details for CMake component `{component}`")

    for component in sorted(doc_rows.keys() - cmake_components.keys()):
        failures.append(f"docs/component-map.md details non-CMake component `{component}`")

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

    valid_surfaces = {"curated", "extended", "complete", "explicit-only", "selected"}
    for component, (target, imports, api_surface, contracts) in sorted(doc_rows.items()):
        if target != component:
            failures.append(
                "component-map row target mismatch for "
                f"`{component}`: expected `conflux::{component}`, found `conflux::{target}`"
            )
        if "conflux" not in imports:
            failures.append(f"component-map row for `{component}` must document an import/include path")
        if api_surface not in valid_surfaces:
            failures.append(
                f"component-map row for `{component}` uses unknown API surface `{api_surface}`"
            )
        if not re.search(r"\b(docs|examples|tests|fuzz|src)/|README\.md", contracts):
            failures.append(f"component-map row for `{component}` must document a contract owner")

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
