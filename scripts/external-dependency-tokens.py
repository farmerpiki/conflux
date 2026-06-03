#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path


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


def main() -> int:
    if len(sys.argv) < 2:
        fail("usage: external-dependency-tokens.py <repo-root> [--exclude TOKEN ...]")
    root = Path(sys.argv[1]).resolve()
    excludes: set[str] = set()
    args = sys.argv[2:]
    if args:
        if args[0] != "--exclude":
            fail("usage: external-dependency-tokens.py <repo-root> [--exclude TOKEN ...]")
        excludes = set(args[1:])

    tokens = external_dependency_tokens(root)
    unknown = sorted(excludes - set(tokens))
    if unknown:
        fail("unknown excluded external dependency tokens: " + ";".join(unknown))
    print(";".join(token for token in tokens if token not in excludes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
