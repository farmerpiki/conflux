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
NAMESPACE_RE = re.compile(r'\bnamespace\s+([A-Za-z_][\w:]*)?\s*(?=[{=])')
GLOBAL_DECL_RE = re.compile(
    r'^\s*(class|struct|enum(?:\s+class)?|using|typedef|concept|constexpr|consteval|constinit|inline|'
    r'auto|void|bool|char|short|int|long|float|double|std::|[A-Za-z_][\w:<>,\s*&]+)\b'
)


def scrub(line: str, in_block_comment: bool) -> tuple[str, bool]:
    out: list[str] = []
    i = 0
    quote = ''
    while i < len(line):
        ch = line[i]
        nxt = line[i + 1] if i + 1 < len(line) else ''
        if in_block_comment:
            if ch == '*' and nxt == '/':
                in_block_comment = False
                i += 2
            else:
                i += 1
            out.append(' ')
            continue
        if quote:
            if ch == '\\':
                out.extend('  ')
                i += 2
                continue
            if ch == quote:
                quote = ''
            out.append(' ')
            i += 1
            continue
        if ch == '/' and nxt == '*':
            in_block_comment = True
            out.extend('  ')
            i += 2
            continue
        if ch == '/' and nxt == '/':
            out.extend(' ' * (len(line) - i))
            break
        if ch in {'"', "'"}:
            quote = ch
            out.append(' ')
            i += 1
            continue
        out.append(ch)
        i += 1
    return ''.join(out), in_block_comment


def namespace_events(line: str) -> list[tuple[int, str]]:
    return [(match.start(), match.group(1) or '') for match in NAMESPACE_RE.finditer(line)]


def allowed_global_declaration(stripped: str) -> bool:
    return (
        not stripped
        or stripped.startswith(('#', 'import ', 'module ', 'namespace ', 'template<', 'template <', 'requires ', 'static_assert'))
        or stripped.startswith('extern ')
        or stripped in {'struct io_uring;', 'struct io_uring_sqe;', 'struct __kernel_timespec;'}
        or stripped.startswith('struct std::')
        or stripped.startswith('class std::')
        or stripped.startswith('struct conflux::')
        or stripped.startswith('class conflux::')
        or stripped.startswith('using std::')
        or stripped.startswith('inline constexpr bool std::')
        or stripped.startswith('inline constexpr auto std::')
    )


def check_no_global_declarations(text: str, rel: str, errors: list[str]) -> None:
    depth = 0
    ns_stack: list[tuple[str, int]] = []
    in_block_comment = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        line, in_block_comment = scrub(raw, in_block_comment)
        stripped = line.strip()
        in_allowed_namespace = any(
            name == 'conflux' or name.startswith('conflux::') or name == 'std' or name.startswith('std::')
            for name, _ in ns_stack
        )
        if depth == 0 and not in_allowed_namespace and GLOBAL_DECL_RE.match(line) and not allowed_global_declaration(stripped):
            errors.append(f'{rel}:{lineno}: top-level public declaration leaked: {raw.strip()}')

        pending = namespace_events(line)
        pending_idx = 0
        for pos, ch in enumerate(line):
            if ch == '{':
                depth += 1
                while pending_idx < len(pending) and pending[pending_idx][0] < pos:
                    name = pending[pending_idx][1]
                    if name:
                        ns_stack.append((name, depth))
                    pending_idx += 1
                    break
            elif ch == '}':
                depth -= 1
                while ns_stack and ns_stack[-1][1] > depth:
                    ns_stack.pop()
                if depth < 0:
                    depth = 0


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


def check_public_header(path: Path, rel: str, check_optional_dependencies: bool, errors: list[str]) -> None:
    text = path.read_text(encoding='utf-8')
    check_no_global_declarations(text, rel, errors)
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if SHORT_ALIAS_RE.match(line):
            errors.append(f'{rel}:{lineno}: public shorthand alias leaked: {stripped}')
        macro = MACRO_RE.match(line)
        if macro and not macro.group(1).startswith('CONFLUX_'):
            errors.append(f'{rel}:{lineno}: non-CONFLUX public macro leaked: {macro.group(1)}')
        if check_optional_dependencies and OPTIONAL_INCLUDE_RE.match(line):
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

    for rel in sorted(headers):
        path = args.include_dir / rel
        if not path.exists():
            errors.append(f'{rel}: expected public header missing from generated include tree')
            continue
        check_public_header(path, rel, rel in CORE_PUBLIC_HEADERS, errors)

    if errors:
        for err in errors:
            print(f'public-header-hygiene: {err}', file=sys.stderr)
        return 1
    print(
        'public-header-hygiene: checked '
        f'{len(headers)} public headers; {len(CORE_PUBLIC_HEADERS & selected_set)} core dependency guards'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
