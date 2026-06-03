#!/usr/bin/env python3
"""Validate a staged conflux preview release artifact."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"check-release-artifact: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_manifest(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep:
            entries[key] = value
    return entries


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        fail("usage: check-release-artifact.py <stage-dir>")

    stage = Path(argv[1]).resolve()
    package_config = next(
        stage.glob("install/lib*/cmake/conflux/conflux-config.cmake"),
        None,
    )
    if package_config is None:
        fail("missing install/lib*/cmake/conflux/conflux-config.cmake")

    required = [
        stage / "source" / "CMakeLists.txt",
        stage / "source" / "CMakePresets.json",
        stage / "source" / "CHANGELOG.md",
        stage / "source" / "LICENSE",
        stage / "source" / "README.md",
        stage / "source" / "RELEASE_POLICY.md",
        stage / "source" / "SECURITY.md",
        stage / "source" / "SUPPORT.md",
        stage / "source" / "examples" / "release-json" / "json.cxx",
        stage / "source" / "examples" / "release-json" / "json_config.cxx",
        stage / "source" / "examples" / "release-json" / "json_diagnostics.cxx",
        stage / "source" / "examples" / "release-json" / "json_stream_ingest.cxx",
        stage / "source" / "examples" / "release-json" / "json_transform.cxx",
        stage / "source" / "include" / "conflux" / "json.hxx",
        stage / "source" / "include" / "conflux" / "features.hxx",
        stage / "source" / "scripts" / "generate-public-header-include-smoke.py",
        stage / "source" / "scripts" / "module_header_bridge.py",
        stage / "source" / "src",
        stage / "install" / "include" / "conflux" / "json.hxx",
        stage / "install" / "include" / "conflux" / "features.hxx",
        stage / "artifacts" / "module-header-bridge-manifest.json",
        stage / "evidence-template.md",
        stage / "release-artifact-manifest.txt",
    ]
    for path in required:
        if not path.exists():
            fail(f"missing {path.relative_to(stage)}")

    release_manifest = read_manifest(stage / "release-artifact-manifest.txt")
    if release_manifest.get("feature_set") != "release-json":
        fail("release artifact must be staged with feature_set=release-json")
    if release_manifest.get("source_generated_header_artifact") != "source/include/conflux":
        fail("release artifact must record source/include/conflux generated headers")
    if release_manifest.get("selected_examples") != "source/examples/release-json":
        fail("release artifact must record release-json selected examples")

    manifest_path = stage / "artifacts" / "module-header-bridge-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    bridge = manifest.get("bridge")
    if not isinstance(bridge, dict):
        fail("bridge manifest does not expose bridge metadata")
    for key in ["script", "python_executable", "python_version", "argv", "options", "stdlib_only"]:
        if key not in bridge:
            fail(f"bridge manifest missing bridge.{key}")
    if bridge.get("stdlib_only") is not True:
        fail("bridge manifest must declare stdlib_only=true")
    if not manifest.get("interfaces"):
        fail("bridge manifest has no generated public interfaces")

    print("check-release-artifact: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
