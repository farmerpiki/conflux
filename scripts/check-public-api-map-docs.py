#!/usr/bin/env python3
"""Guard the prerelease public API freeze map."""
from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    api_map = read("docs/public-api-map.md")
    profiles = read("docs/api-surface-profiles.md")
    component_map = read("docs/component-map.md")
    release_checklist = read("docs/release-checklist.md")

    required_api_map_markers = [
        "# Public API map",
        "Stable candidate",
        "Advanced",
        "Low-level public",
        "experimental or internal/detail",
        "cost-lifetime-model.md",
        "api-surface-profiles.md",
        "import conflux;",
        "import conflux.curated;",
        "import conflux.http;",
        "import conflux.json;",
        "import conflux.extended;",
        "import conflux.complete;",
        "import conflux.uring;",
        "`CONFLUX_FEATURE_SET` selects what is built.",
        "`CONFLUX_API_SURFACE` selects what",
    ]
    for marker in required_api_map_markers:
        if marker not in api_map:
            failures.append(f"docs/public-api-map.md missing {marker!r}")

    for phrase in [
        "deprecated compatibility alias",
        "compatibility aliases are available",
        "compatibility aliases remain",
    ]:
        if phrase in api_map.lower():
            failures.append(f"docs/public-api-map.md must not advertise {phrase!r}")

    required_profile_markers = [
        "Profile aggregate exports are guarded by",
        "api-surface-manifest.json",
        "`complete` means complete documented public surface, not private internals.",
    ]
    for marker in required_profile_markers:
        if marker not in profiles:
            failures.append(f"docs/api-surface-profiles.md missing {marker!r}")

    required_component_markers = [
        "`STABLE`, `ADVANCED`, `EXPERIMENTAL`, or `INTERNAL_SUPPORT`",
        "Only registry entries with `REQUESTABLE` kind",
        "`find_package(... COMPONENTS ...)`",
    ]
    for marker in required_component_markers:
        if marker not in component_map:
            failures.append(f"docs/component-map.md missing {marker!r}")

    if "Confirm no deprecated public compatibility aliases are advertised" not in release_checklist:
        failures.append("docs/release-checklist.md must keep alias cleanup evidence requirement")

    if failures:
        print("public API map docs guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("public API map docs: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
