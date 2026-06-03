#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"release-sku-field: {message}", file=sys.stderr)
    raise SystemExit(1)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        fail("usage: release-sku-field.py <repo-root> <sku> <feature_set|components|docs|examples>")
    root = Path(argv[1]).resolve()
    sku_name = argv[2]
    field = argv[3]
    manifest_path = root / "docs" / "release-skus.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    sku = manifest.get(sku_name)
    if not isinstance(sku, dict):
        fail(f"unknown release SKU: {sku_name}")
    value = sku.get(field)
    if field == "feature_set":
        if not isinstance(value, str) or not value:
            fail(f"{sku_name}.{field} must be a non-empty string")
        print(value)
        return 0
    if field not in {"components", "docs", "examples"}:
        fail(f"unknown field: {field}")
    if not isinstance(value, list) or not value or not all(isinstance(item, str) and item for item in value):
        fail(f"{sku_name}.{field} must be a non-empty string list")
    for item in value:
        if field in {"docs", "examples"} and not (root / item).is_file():
            fail(f"{sku_name}.{field} references missing file: {item}")
    if field == "components":
        print(";".join(value))
    else:
        print("\n".join(value))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
