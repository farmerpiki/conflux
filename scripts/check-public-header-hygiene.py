#!/usr/bin/env python3
"""Check generated public headers for release-facing hygiene regressions."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

CORE_PUBLIC_HEADERS = {
    'conflux/types.hxx',
    'conflux/types/api.hxx',
    'conflux/utils.hxx',
    'conflux/small_function.hxx',
    'conflux/config.hxx',
    'conflux/json.hxx',
    'conflux/json/api.hxx',
    'conflux/http.hxx',
    'conflux/net/config.hxx',
    'conflux/net/http.hxx',
    'conflux/net/http/types.hxx',
    'conflux/net/http/request.hxx',
    'conflux/net/http/response.hxx',
    'conflux/net/router.hxx',
}

SHORT_ALIAS_RE = re.compile(r'^(?:export\s+)?using\s+(S|SV|V|M|SP|UP|Opt|Fn|Tup|RE|EC|SZ)\s*=')
MACRO_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\b')
OPTIONAL_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*<[^>]*(liburing|openssl/|libpq-fe|nghttp2|nghttp3|ngtcp2|zlib|brotli|zstd|libdeflate|isal|argon2)[^>]*>'
)


def load_manifest_headers(manifest: Path) -> list[str]:
    data = json.loads(manifest.read_text(encoding='utf-8'))
    out: list[str] = []
    seen: set[str] = set()
    for item in data.get('interfaces', []):
        rel = item.get('header_relpath')
        if not isinstance(rel, str) or not rel.endswith('.hxx'):
            continue
        if rel.startswith('conflux/detail/') or '/detail/' in rel:
            continue
        if rel in seen:
            continue
        seen.add(rel)
        out.append(rel)
    return out


def selected(headers: list[str], skip_headers: set[str], skip_prefixes: tuple[str, ...]) -> list[str]:
    return [h for h in headers if h not in skip_headers and not h.startswith(skip_prefixes)]


def check_core_header(path: Path, rel: str, errors: list[str]) -> None:
    text = path.read_text(encoding='utf-8')
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if SHORT_ALIAS_RE.match(line):
            errors.append(f'{rel}:{lineno}: public shorthand alias leaked: {stripped}')
        macro = MACRO_RE.match(line)
        if macro and not macro.group(1).startswith('CONFLUX_'):
            errors.append(f'{rel}:{lineno}: non-CONFLUX public macro leaked: {macro.group(1)}')
        if OPTIONAL_INCLUDE_RE.match(line):
            errors.append(f'{rel}:{lineno}: core/public convenience header pulls optional dependency: {stripped}')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--include-dir', required=True, type=Path)
    parser.add_argument('--skip-prefix', action='append', default=[])
    parser.add_argument('--skip-header', action='append', default=[])
    args = parser.parse_args()

    headers = selected(load_manifest_headers(args.manifest), set(args.skip_header), tuple(args.skip_prefix))
    selected_set = set(headers)
    errors: list[str] = []

    for rel in sorted(CORE_PUBLIC_HEADERS & selected_set):
        path = args.include_dir / rel
        if not path.exists():
            errors.append(f'{rel}: expected public header missing from generated include tree')
            continue
        check_core_header(path, rel, errors)

    if errors:
        for err in errors:
            print(f'public-header-hygiene: {err}', file=sys.stderr)
        return 1
    print(f'public-header-hygiene: checked {len(CORE_PUBLIC_HEADERS & selected_set)} core public headers')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
