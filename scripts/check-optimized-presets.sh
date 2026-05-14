#!/usr/bin/env bash
# Static guard for optimized build presets: LTO/PGO presets are allowed to be
# expensive and non-recordable, but must stay explicit and unsanitized.
set -euo pipefail

SOURCE_DIR="${1:-}"
if [[ -z "$SOURCE_DIR" ]]; then
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    SOURCE_DIR="$(cd "$script_dir/.." && pwd)"
fi

python3 - "$SOURCE_DIR/CMakePresets.json" <<'PY'
from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

path = Path(sys.argv[1])
data: dict[str, Any] = json.loads(path.read_text())
configure = {p["name"]: p for p in data.get("configurePresets", [])}
build = {p["name"]: p for p in data.get("buildPresets", [])}

errors: list[str] = []
seen: set[str] = set()

def merged_cache(name: str, stack: tuple[str, ...] = ()) -> dict[str, str]:
    if name in stack:
        errors.append(f"configure preset inheritance cycle: {' -> '.join(stack + (name,))}")
        return {}
    preset = configure.get(name)
    if preset is None:
        errors.append(f"missing configure preset: {name}")
        return {}

    out: dict[str, str] = {}
    parents = preset.get("inherits", [])
    if isinstance(parents, str):
        parents = [parents]
    for parent in parents:
        out.update(merged_cache(parent, stack + (name,)))

    for key, value in preset.get("cacheVariables", {}).items():
        if isinstance(value, dict):
            value = value.get("value", "")
        out[key] = str(value)
    return out

def require_build_preset(name: str) -> None:
    bp = build.get(name)
    if bp is None:
        errors.append(f"missing build preset: {name}")
        return
    if bp.get("configurePreset") != name:
        errors.append(f"build preset {name} must point at configure preset {name}")

def require(cache: dict[str, str], name: str, key: str, expected: str) -> None:
    actual = cache.get(key, "")
    if actual != expected:
        errors.append(f"{name}: expected {key}={expected}, got {actual or '<unset>'}")

def require_unsanitized(cache: dict[str, str], name: str) -> None:
    for key in ("CONFLUX_ENABLE_ASAN", "CONFLUX_ENABLE_UBSAN", "CONFLUX_ENABLE_TSAN"):
        actual = cache.get(key, "OFF")
        if actual != "OFF":
            errors.append(f"{name}: optimized preset must keep {key}=OFF, got {actual}")

def check_release(name: str, lto: str, lto_mode: str | None) -> None:
    seen.add(name)
    require_build_preset(name)
    cache = merged_cache(name)
    require(cache, name, "CMAKE_BUILD_TYPE", "Release")
    require(cache, name, "CONFLUX_BUILD_TESTS", "ON")
    require(cache, name, "CONFLUX_BUILD_BENCHMARKS", "ON")
    require(cache, name, "CONFLUX_ENABLE_LTO", lto)
    require_unsanitized(cache, name)
    if lto == "ON":
        actual = cache.get("CONFLUX_LTO_MODE", "AUTO")
        if lto_mode is not None and actual != lto_mode:
            errors.append(f"{name}: expected CONFLUX_LTO_MODE={lto_mode}, got {actual}")
        if actual not in {"AUTO", "THIN", "FULL"}:
            errors.append(f"{name}: invalid CONFLUX_LTO_MODE={actual}")

def check_pgo(name: str, generate: bool, clang: bool) -> None:
    seen.add(name)
    require_build_preset(name)
    cache = merged_cache(name)
    require(cache, name, "CMAKE_BUILD_TYPE", "Release")
    require(cache, name, "CONFLUX_BUILD_TESTS", "ON")
    require(cache, name, "CONFLUX_BUILD_BENCHMARKS", "ON")
    require_unsanitized(cache, name)
    profile = cache.get("CONFLUX_PGO_PROFILE_DIR", "")
    if not profile:
        errors.append(f"{name}: CONFLUX_PGO_PROFILE_DIR must be explicit")
    if generate:
        require(cache, name, "CONFLUX_PGO_GENERATE", "ON")
    elif cache.get("CONFLUX_PGO_GENERATE", "OFF") == "ON":
        errors.append(f"{name}: PGO use preset must not set CONFLUX_PGO_GENERATE=ON")
    if clang:
        if generate and ".profraw" not in profile:
            errors.append(f"{name}: Clang generate profile path should be a .profraw pattern")
        if not generate and not profile.endswith(".profdata"):
            errors.append(f"{name}: Clang use profile path should be merged .profdata")
    else:
        if profile.endswith((".profraw", ".profdata")):
            errors.append(f"{name}: GCC profile path should be a profile directory")

check_release("release-clang-libcxx", "ON", "THIN")
check_release("release-clang-stdcxx", "ON", "THIN")
check_release("release-gcc-stdcxx", "OFF", None)
check_release("release-gcc16-stdcxx", "ON", "AUTO")

check_pgo("pgo-gen-clang-libcxx", generate=True, clang=True)
check_pgo("pgo-use-clang-libcxx", generate=False, clang=True)
check_pgo("pgo-gen-gcc-stdcxx", generate=True, clang=False)
check_pgo("pgo-use-gcc-stdcxx", generate=False, clang=False)
check_pgo("pgo-gen-gcc16-stdcxx", generate=True, clang=False)
check_pgo("pgo-use-gcc16-stdcxx", generate=False, clang=False)

for name in sorted(n for n in configure if n.startswith(("release-", "pgo-"))):
    if name not in seen and name.endswith(("-libcxx", "-stdcxx")):
        errors.append(f"optimized preset is not covered by this guard: {name}")

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    raise SystemExit(1)

print("optimized preset guard passed")
PY
