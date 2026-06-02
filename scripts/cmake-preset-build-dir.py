#!/usr/bin/env python3
import json
import sys
from pathlib import Path
from typing import Any


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def load_preset_file(path: Path, seen: set[Path] | None = None) -> list[dict[str, Any]]:
    path = path.resolve()
    if seen is None:
        seen = set()
    if path in seen:
        fail(f"cyclic preset include involving {path}")
    seen.add(path)

    data = json.loads(path.read_text(encoding="utf-8"))
    presets: list[dict[str, Any]] = []
    for include in data.get("include", []):
        if not isinstance(include, str):
            fail(f"{path} has non-string preset include")
        presets.extend(load_preset_file(path.parent / include, seen.copy()))
    presets.extend(data.get("configurePresets", []))
    return presets


def load_presets(path: Path) -> dict[str, dict[str, Any]]:
    presets: dict[str, dict[str, Any]] = {}
    for preset in load_preset_file(path):
        name = preset.get("name")
        if name is None:
            continue
        if not isinstance(name, str):
            fail(f"{path} has configure preset with non-string name")
        if name in presets:
            fail(f"duplicate configure preset: {name}")
        presets[name] = preset
    return presets


def resolve_field(
    presets: dict[str, dict[str, Any]],
    name: str,
    field: str,
    seen: set[str] | None = None,
) -> str | None:
    if seen is None:
        seen = set()
    if name in seen:
        fail(f"cyclic preset inheritance involving {name}")
    seen.add(name)

    preset = presets.get(name)
    if preset is None:
        fail(f"unknown configure preset: {name}")
    if field in preset:
        value = preset[field]
        if not isinstance(value, str):
            fail(f"preset {name} has non-string {field}")
        return value

    inherits = preset.get("inherits", [])
    if isinstance(inherits, str):
        inherits = [inherits]
    if not isinstance(inherits, list):
        fail(f"preset {name} has invalid inherits field")

    for parent in inherits:
        if not isinstance(parent, str):
            fail(f"preset {name} has non-string inherited preset")
        value = resolve_field(presets, parent, field, seen.copy())
        if value is not None:
            return value
    return None


def expand_binary_dir(template: str, source_dir: Path, preset_name: str) -> str:
    source_dir = source_dir.resolve()
    replacements = {
        "${sourceDir}": str(source_dir),
        "${sourceParentDir}": str(source_dir.parent),
        "${sourceDirName}": source_dir.name,
        "${presetName}": preset_name,
        "${fileDir}": str(source_dir),
    }
    for key, value in replacements.items():
        template = template.replace(key, value)
    return template


def main() -> None:
    if len(sys.argv) != 3:
        fail(f"usage: {sys.argv[0]} <source-dir> <configure-preset>")

    source_dir = Path(sys.argv[1]).resolve()
    presets = load_presets(source_dir / "CMakePresets.json")
    preset_name = sys.argv[2]
    binary_dir = resolve_field(presets, preset_name, "binaryDir")
    if binary_dir is None:
        fail(f"preset {preset_name} does not define binaryDir through inheritance")
    print(expand_binary_dir(binary_dir, source_dir, preset_name))


if __name__ == "__main__":
    main()
