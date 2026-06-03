#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKU_MANIFEST = ROOT / "docs" / "release-skus.json"
MODULE_EXAMPLES = ROOT / "examples" / "CMakeLists.txt"
HEADER_INTERFACE = ROOT / "cmake" / "ConfluxInterfaceMode.cmake"


def fail(message: str) -> None:
    print(f"release-sku-examples: {message}", file=sys.stderr)
    raise SystemExit(1)


def module_example_sources() -> set[str]:
    text = MODULE_EXAMPLES.read_text(encoding="utf-8")
    return {
        f"examples/{match}"
        for match in re.findall(
            r"add_executable\([^)]*\$\{_conflux_examples_root\}/([A-Za-z0-9_./-]+\.cxx)",
            text,
            re.DOTALL,
        )
    }


def header_example_sources() -> set[str]:
    text = HEADER_INTERFACE.read_text(encoding="utf-8")
    return {
        f"{match}.cxx"
        for match in re.findall(
            r"conflux_add_header_example_from_id\([^)]*\b(examples/[A-Za-z0-9_./-]+)",
            text,
            re.DOTALL,
        )
    }


def main() -> int:
    manifest = json.loads(SKU_MANIFEST.read_text(encoding="utf-8"))
    module_examples = module_example_sources()
    header_examples = header_example_sources()
    errors: list[str] = []

    if not isinstance(manifest, dict):
        fail("release SKU manifest must be a JSON object")

    for sku_name, sku in sorted(manifest.items()):
        if not isinstance(sku, dict):
            errors.append(f"{sku_name}: entry must be an object")
            continue
        examples = sku.get("examples")
        if not isinstance(examples, list):
            errors.append(f"{sku_name}: examples must be a list")
            continue
        for example in examples:
            if not isinstance(example, str):
                errors.append(f"{sku_name}: example path must be a string")
                continue
            if example not in module_examples:
                errors.append(f"{sku_name}: {example} is not declared in examples/CMakeLists.txt")
            if example not in header_examples:
                errors.append(f"{sku_name}: {example} is not declared in header-mode examples")

    if errors:
        print("release-sku-examples guard failed:", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("release-sku-examples: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
