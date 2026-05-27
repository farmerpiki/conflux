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
PUBLIC_COMPONENT_RE = re.compile(r'"([^"|]+)\|([^"|]+)"')
DOC_COMPONENT_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`conflux::([^`]+)`\s*\|",
    re.MULTILINE,
)


def public_components() -> dict[str, str]:
    text = COMPONENT_SOURCE.read_text(encoding="utf-8")
    block = PUBLIC_BLOCK_RE.search(text)
    if block is None:
        raise ValueError("missing CONFLUX_PUBLIC_COMPONENT_DECLARATIONS")
    components: dict[str, str] = {}
    for target, export_name in PUBLIC_COMPONENT_RE.findall(block.group("body")):
        if export_name in components:
            raise ValueError(f"duplicate CMake public component: {export_name}")
        components[export_name] = target
    return components


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


def main() -> int:
    failures: list[str] = []
    cmake_components = public_components()
    doc_components = documented_components()

    for component in sorted(cmake_components.keys() - doc_components.keys()):
        failures.append(f"missing docs/component-map.md row for CMake component `{component}`")

    for component in sorted(doc_components.keys() - cmake_components.keys()):
        failures.append(f"docs/component-map.md documents non-CMake component `{component}`")

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
