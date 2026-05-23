#!/usr/bin/env python3
"""Generate compile-only import smoke sources for public module file sets."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

MODULE_RE = re.compile(r"^\s*export\s+module\s+([A-Za-z_][A-Za-z0-9_.]*(?::[A-Za-z_][A-Za-z0-9_]*)?)\s*;")


def cmake_quote(path: Path) -> str:
    return '"' + str(path).replace('\\', '/').replace('"', '\\"') + '"'


def symbol_for(module: str) -> str:
    safe = re.sub(r'[^A-Za-z0-9_]', '_', module)
    return f'conflux_public_module_smoke_{safe}'


def module_name_from(path: Path) -> str | None:
    try:
        text = path.read_text(encoding='utf-8')
    except UnicodeDecodeError:
        text = path.read_text(encoding='utf-8', errors='ignore')
    for line in text.splitlines():
        match = MODULE_RE.match(line)
        if match:
            return match.group(1)
    return None


def read_source_list(path: Path) -> list[Path]:
    if not path.exists():
        return []
    return [Path(line.strip()) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-list', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    parser.add_argument('--cmake-fragment', required=True, type=Path)
    parser.add_argument('--skip-module', action='append', default=[])
    parser.add_argument('--skip-prefix', action='append', default=[])
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    args.cmake_fragment.parent.mkdir(parents=True, exist_ok=True)

    skipped_modules = set(args.skip_module)
    skipped_prefixes = tuple(args.skip_prefix)
    seen: set[str] = set()
    modules: list[str] = []
    sources: list[Path] = []
    missing: list[Path] = []

    for source in read_source_list(args.source_list):
        if not source.exists():
            missing.append(source)
            continue
        module = module_name_from(source)
        if not module:
            continue
        if ':' in module:
            continue
        if not module.startswith('conflux'):
            continue
        if module in skipped_modules or module.startswith(skipped_prefixes):
            continue
        if module in seen:
            continue
        seen.add(module)
        modules.append(module)

    for module in sorted(modules):
        source = args.out_dir / f'{symbol_for(module)}.cxx'
        source.write_text(
            f'import {module};\n'
            f'int {symbol_for(module)}() noexcept {{ return 0; }}\n',
            encoding='utf-8')
        sources.append(source)

    with args.cmake_fragment.open('w', encoding='utf-8') as out:
        out.write('set(CONFLUX_PUBLIC_MODULE_SMOKE_SOURCES\n')
        for source in sources:
            out.write(f'    {cmake_quote(source)}\n')
        out.write(')\n')
        out.write('set(CONFLUX_PUBLIC_MODULE_SMOKE_MODULES\n')
        for module in sorted(modules):
            out.write(f'    "{module}"\n')
        out.write(')\n')
        out.write(f'set(CONFLUX_PUBLIC_MODULE_SMOKE_COUNT {len(sources)})\n')

    if missing:
        for path in missing:
            print(f'public-module-import-smoke: missing source: {path}')
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
