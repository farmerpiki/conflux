#!/usr/bin/env python3
"""Generate compile-only include smoke sources for generated public headers."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def cmake_quote(path: Path) -> str:
    return '"' + str(path).replace('\\', '/').replace('"', '\\"') + '"'


def symbol_for(relpath: str) -> str:
    stem = relpath.removesuffix('.hxx')
    safe = re.sub(r'[^A-Za-z0-9_]', '_', stem)
    return f'conflux_public_header_smoke_{safe}'


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    parser.add_argument('--cmake-fragment', required=True, type=Path)
    parser.add_argument('--skip-prefix', action='append', default=[])
    parser.add_argument('--skip-header', action='append', default=[])
    args = parser.parse_args()

    data = json.loads(args.manifest.read_text(encoding='utf-8'))
    args.out_dir.mkdir(parents=True, exist_ok=True)
    args.cmake_fragment.parent.mkdir(parents=True, exist_ok=True)

    seen: set[str] = set()
    skipped_headers = set(args.skip_header)
    skipped_prefixes = tuple(args.skip_prefix)
    sources: list[Path] = []
    for item in data.get('interfaces', []):
        relpath = item.get('header_relpath')
        if not isinstance(relpath, str) or not relpath.endswith('.hxx'):
            continue
        if relpath.startswith('conflux/detail/') or '/detail/' in relpath:
            continue
        if relpath in skipped_headers or relpath.startswith(skipped_prefixes):
            continue
        if relpath in seen:
            continue
        seen.add(relpath)
        source = args.out_dir / (symbol_for(relpath) + '.cxx')
        source.write_text(
            f'#include <{relpath}>\n'
            f'int {symbol_for(relpath)}() noexcept {{ return 0; }}\n',
            encoding='utf-8')
        sources.append(source)

    with args.cmake_fragment.open('w', encoding='utf-8') as out:
        out.write('set(CONFLUX_PUBLIC_HEADER_SMOKE_SOURCES\n')
        for source in sources:
            out.write(f'    {cmake_quote(source)}\n')
        out.write(')\n')
        out.write(f'set(CONFLUX_PUBLIC_HEADER_SMOKE_COUNT {len(sources)})\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
