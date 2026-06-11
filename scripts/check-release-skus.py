#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from component_registry import exports

ROOT = Path(__file__).resolve().parents[1]
SKU_MANIFEST = ROOT / "docs" / "release-skus.json"
PRESETS = ROOT / "cmake" / "ConfluxPresets.cmake"


def public_components() -> set[str]:
    return exports(ROOT, "REQUESTABLE")


def feature_sets() -> set[str]:
    text = PRESETS.read_text(encoding="utf-8")
    match = re.search(
        r"set_property\(CACHE CONFLUX_FEATURE_SET PROPERTY STRINGS(?P<body>.*?)\)",
        text,
        re.DOTALL,
    )
    if match is None:
        raise ValueError("missing CONFLUX_FEATURE_SET cache string list")
    return set(re.findall(r"\b[A-Za-z][A-Za-z0-9_-]*\b", match.group("body")))


def string_list(value: object, errors: list[str], field: str) -> list[str]:
    if not isinstance(value, list):
        errors.append(f"{field} must be a list")
        return []
    strings: list[str] = []
    for item in value:
        if not isinstance(item, str) or not item:
            errors.append(f"{field} must contain only non-empty strings")
            continue
        strings.append(item)
    return strings


def check_selected_file(item: str, field: str, sku_name: str, errors: list[str]) -> None:
    root_name = "docs" if field == "docs" else "examples"
    root = (ROOT / root_name).resolve()
    path = (ROOT / item).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        errors.append(f"{sku_name}: {field[:-1]} path must live under {root_name}/: {item}")
        return
    if not path.is_file():
        errors.append(f"{sku_name}: missing {field} path {item}")


def main() -> int:
    manifest = json.loads(SKU_MANIFEST.read_text(encoding="utf-8"))
    components = public_components()
    known_feature_sets = feature_sets()
    required_skus = {"release-json", "release-http-api", "release-web-server"}
    errors: list[str] = []

    if not isinstance(manifest, dict):
        errors.append("release SKU manifest must be a JSON object")
    else:
        missing_skus = sorted(required_skus - manifest.keys())
        if missing_skus:
            errors.append("missing release SKUs: " + ";".join(missing_skus))
        for sku_name, sku in sorted(manifest.items()):
            if not isinstance(sku, dict):
                errors.append(f"{sku_name}: entry must be an object")
                continue
            feature_set = sku.get("feature_set")
            if not isinstance(feature_set, str) or not feature_set:
                errors.append(f"{sku_name}: feature_set must be a non-empty string")
            elif feature_set not in known_feature_sets:
                errors.append(f"{sku_name}: unknown feature_set {feature_set}")
            elif feature_set != sku_name:
                errors.append(f"{sku_name}: feature_set must match the SKU name")

            selected_components = string_list(sku.get("components"), errors, f"{sku_name}: components")
            seen_components: set[str] = set()
            for component in selected_components:
                if component in seen_components:
                    errors.append(f"{sku_name}: duplicate component {component}")
                seen_components.add(component)
                if component not in components:
                    errors.append(f"{sku_name}: component {component} is not requestable")
            if not selected_components:
                errors.append(f"{sku_name}: components must be a non-empty list")

            for field in ["docs", "examples"]:
                paths = string_list(sku.get(field), errors, f"{sku_name}: {field}")
                if not paths:
                    errors.append(f"{sku_name}: {field} must be a non-empty list")
                    continue
                seen: set[str] = set()
                seen_basenames: dict[str, str] = {}
                for item in paths:
                    if item in seen:
                        errors.append(f"{sku_name}: duplicate {field} path {item}")
                    seen.add(item)
                    basename = Path(item).name
                    previous = seen_basenames.get(basename)
                    if previous is not None:
                        errors.append(
                            f"{sku_name}: duplicate staged {field} basename {basename}: "
                            f"{previous} and {item}"
                        )
                    seen_basenames[basename] = item
                    check_selected_file(item, field, sku_name, errors)

    if errors:
        print("release-skus guard failed:", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("release-skus: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
