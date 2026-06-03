#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


POLICY_ALLOWED_TOKENS = {
    "all": [],
    "core": [],
    "json": ["XXHASH"],
    "template": ["XXHASH", "OPENSSL", "ZLIB", "LIBDEFLATE", "ZLIB_NG", "LIBISAL", "BROTLI", "ZSTD"],
    "dns": ["LIBURING", "XXHASH"],
    "pg": ["LIBURING", "XXHASH", "LIBPQ"],
}


def fail(message: str) -> None:
    print(f"external-dependency-tokens: {message}", file=sys.stderr)
    raise SystemExit(1)


def external_dependency_tokens(root: Path) -> list[str]:
    registry = root / "cmake" / "ConfluxExternalDependencyRegistry.cmake"
    text = registry.read_text(encoding="utf-8")
    match = re.search(
        r"set\(CONFLUX_EXTERNAL_DEPENDENCY_TOKENS(?P<body>.*?)\)",
        text,
        re.DOTALL,
    )
    if match is None:
        fail("missing CONFLUX_EXTERNAL_DEPENDENCY_TOKENS registry")
    tokens = re.findall(r"\b[A-Z][A-Z0-9_]*\b", match.group("body"))
    if not tokens:
        fail("external dependency token registry is empty")
    return tokens


def validate_policies(tokens: list[str]) -> None:
    known = set(tokens)
    errors: list[str] = []
    for policy, allowed_tokens in sorted(POLICY_ALLOWED_TOKENS.items()):
        seen: set[str] = set()
        for token in allowed_tokens:
            if token in seen:
                errors.append(f"{policy}: duplicate allowed token {token}")
            seen.add(token)
            if token not in known:
                errors.append(f"{policy}: unknown allowed token {token}")
    if errors:
        fail("invalid named external dependency policies: " + ";".join(errors))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo_root")
    parser.add_argument("--exclude", nargs="*", default=[])
    parser.add_argument("--policy", choices=sorted(POLICY_ALLOWED_TOKENS))
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    tokens = external_dependency_tokens(root)
    validate_policies(tokens)
    excludes = set(args.exclude)
    if args.policy is not None:
        excludes.update(POLICY_ALLOWED_TOKENS[args.policy])
    unknown = sorted(excludes - set(tokens))
    if unknown:
        fail("unknown excluded external dependency tokens: " + ";".join(unknown))
    print(";".join(token for token in tokens if token not in excludes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
