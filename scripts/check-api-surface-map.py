#!/usr/bin/env python3
"""Fail when API-surface profile exports drift from docs/component-map.md."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPONENT_MAP = ROOT / "docs" / "component-map.md"
PROFILE_SOURCES = {
    "curated": ROOT / "src" / "facade" / "conflux_curated.cxx",
    "extended": ROOT / "src" / "facade" / "conflux_extended.cxx",
    "complete": ROOT / "src" / "facade" / "conflux_complete.cxx",
}
SURFACE_RANK = {
    "curated": 1,
    "selected": 1,
    "extended": 2,
    "complete": 3,
    "explicit-only": 4,
    "internal": 5,
}
PROFILE_MODULES = {
    "conflux.curated",
    "conflux.extended",
    "conflux.complete",
    "conflux.features",
}
DOC_ROW_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`conflux::[^`]+`\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|",
    re.MULTILINE,
)
IMPORT_RE = re.compile(r"\bconflux(?:\.[A-Za-z0-9_]+)+\b")
EXPORT_IMPORT_RE = re.compile(r"^\s*export\s+import\s+(conflux(?:\.[A-Za-z0-9_]+)+)\s*;", re.MULTILINE)


def documented_surfaces() -> dict[str, str]:
    text = COMPONENT_MAP.read_text(encoding="utf-8")
    surfaces: dict[str, str] = {}
    for component, imports_cell, surface_cell in DOC_ROW_RE.findall(text):
        surface = surface_cell.strip().strip("`")
        if surface not in SURFACE_RANK:
            continue
        modules = set(IMPORT_RE.findall(imports_cell))
        if not modules:
            continue
        for module in modules:
            existing = surfaces.get(module)
            if existing and SURFACE_RANK[existing] <= SURFACE_RANK[surface]:
                continue
            surfaces[module] = surface
    return surfaces


def profile_exports() -> dict[str, set[str]]:
    exports: dict[str, set[str]] = {}
    for profile, path in PROFILE_SOURCES.items():
        text = path.read_text(encoding="utf-8")
        exports[profile] = set(EXPORT_IMPORT_RE.findall(text))
    return exports


def main() -> int:
    failures: list[str] = []
    surfaces = documented_surfaces()
    exports = profile_exports()

    for profile, modules in exports.items():
        profile_rank = SURFACE_RANK[profile]
        for module in sorted(modules - PROFILE_MODULES):
            surface = surfaces.get(module)
            if surface is None:
                failures.append(
                    f"{PROFILE_SOURCES[profile].relative_to(ROOT)} exports undocumented module `{module}`"
                )
                continue
            surface_rank = SURFACE_RANK[surface]
            if surface_rank > profile_rank:
                failures.append(
                    f"{PROFILE_SOURCES[profile].relative_to(ROOT)} exports `{module}` "
                    f"from `{profile}`, but component-map classifies it as `{surface}`"
                )

    if failures:
        print("api-surface map guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("api-surface map: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"api-surface map guard failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
