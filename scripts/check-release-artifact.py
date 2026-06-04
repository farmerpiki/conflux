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


def release_sku(manifest: dict[str, str]) -> str:
    sku = manifest.get("release_sku")
    if not sku:
        fail("release artifact must record release_sku")
    return sku


def selected_names(paths: list[str], root: str, field: str, sku: str) -> list[str]:
    names: list[str] = []
    seen: set[str] = set()
    for source_path in paths:
        if not source_path.startswith(root):
            fail(f"staged release SKU manifest has {field} outside {root} for {sku}: {source_path}")
        name = Path(source_path).name
        if name in seen:
            fail(f"staged release SKU manifest has duplicate selected {field} basename for {sku}: {name}")
        seen.add(name)
        names.append(name)
    return names


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
        stage / "source" / "NOTICE",
        stage / "source" / "README.md",
        stage / "source" / "RELEASE_POLICY.md",
        stage / "source" / "SECURITY.md",
        stage / "source" / "SUPPORT.md",
        stage / "source" / "include" / "conflux" / "json.hxx",
        stage / "source" / "include" / "conflux" / "features.hxx",
        stage / "source" / "docs" / "release-skus.json",
        stage / "source" / "scripts" / "generate-public-header-include-smoke.py",
        stage / "source" / "scripts" / "package-smoke-forbidden-components.py",
        stage / "source" / "scripts" / "release-sku-field.py",
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
    for forbidden_key in ["source_root", "build_dir", "stage_dir"]:
        if forbidden_key in release_manifest:
            fail(f"release artifact manifest must not record local {forbidden_key}")
    sku = release_sku(release_manifest)
    sku_manifest = json.loads((stage / "source" / "docs" / "release-skus.json").read_text(encoding="utf-8"))
    sku_entry = sku_manifest.get(sku)
    if not isinstance(sku_entry, dict):
        fail(f"staged release SKU manifest does not define {sku}")
    expected_feature_set = sku_entry.get("feature_set")
    if not isinstance(expected_feature_set, str) or not expected_feature_set:
        fail(f"staged release SKU manifest has invalid feature_set for {sku}")
    if release_manifest.get("feature_set") != expected_feature_set:
        fail(f"release artifact must be staged with feature_set={expected_feature_set}")
    expected_components = sku_entry.get("components")
    if not isinstance(expected_components, list) or not all(isinstance(item, str) and item for item in expected_components):
        fail(f"staged release SKU manifest has invalid components for {sku}")
    if release_manifest.get("package_components") != ";".join(expected_components):
        fail(f"release artifact must record {sku} package components")
    if release_manifest.get("source_generated_header_artifact") != "source/include/conflux":
        fail("release artifact must record source/include/conflux generated headers")
    if release_manifest.get("selected_examples") != f"source/examples/{sku}":
        fail(f"release artifact must record {sku} selected examples")
    if release_manifest.get("selected_docs") != f"source/docs/{sku}":
        fail(f"release artifact must record {sku} selected docs")
    expected_docs = sku_entry.get("docs")
    if not isinstance(expected_docs, list) or not all(isinstance(item, str) and item for item in expected_docs):
        fail(f"staged release SKU manifest has invalid docs for {sku}")
    expected_examples = sku_entry.get("examples")
    if not isinstance(expected_examples, list) or not all(isinstance(item, str) and item for item in expected_examples):
        fail(f"staged release SKU manifest has invalid examples for {sku}")
    for name in selected_names(expected_docs, "docs/", "docs", sku):
        selected = stage / "source" / "docs" / sku / name
        if not selected.is_file():
            fail(f"missing selected release doc {selected.relative_to(stage)}")
    for name in selected_names(expected_examples, "examples/", "examples", sku):
        selected = stage / "source" / "examples" / sku / name
        if not selected.is_file():
            fail(f"missing selected release example {selected.relative_to(stage)}")

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
