#!/usr/bin/env python3
"""Fail when CMake references source-tree files that do not exist."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE_FILES = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").rglob("*.cmake"))]
SOURCE_PREFIXES = (
    "src/",
    "tests/",
    "examples/",
    "benchmarks/",
    "fuzz/",
    "cmake/",
    "configs/",
    "docs/",
    "scripts/",
    "todo/",
    "proposals/",
)
SOURCE_EXTENSIONS = (
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".ixx",
    ".cmake",
    ".cmake.in",
    ".cxx.in",
    ".h.in",
    ".json",
    ".md",
    ".py",
    ".sh",
    ".sql",
    ".txt",
)
TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_./-])([A-Za-z0-9_./+-]+\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|ixx|cmake(?:\.in)?|cxx\.in|h\.in|json|md|py|sh|sql|txt))(?![A-Za-z0-9_./-])")


def strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        in_quote = False
        escaped = False
        kept: list[str] = []
        for char in line:
            if escaped:
                kept.append(char)
                escaped = False
                continue
            if char == "\\":
                kept.append(char)
                escaped = True
                continue
            if char == '"':
                in_quote = not in_quote
                kept.append(char)
                continue
            if char == "#" and not in_quote:
                break
            kept.append(char)
        out.append("".join(kept))
    return "\n".join(out)


def is_source_tree_path(token: str) -> bool:
    return token.startswith(SOURCE_PREFIXES) and token.endswith(SOURCE_EXTENSIONS)


def main() -> int:
    failures: list[str] = []
    seen: set[tuple[Path, str]] = set()

    for cmake_file in CMAKE_FILES:
        if not cmake_file.exists():
            continue
        rel_cmake = cmake_file.relative_to(ROOT)
        text = strip_comments(cmake_file.read_text(errors="ignore"))
        for match in TOKEN_RE.finditer(text):
            token = match.group(1)
            if "${" in token or "$<" in token:
                continue
            if not is_source_tree_path(token):
                continue
            key = (cmake_file, token)
            if key in seen:
                continue
            seen.add(key)
            if not (ROOT / token).exists():
                line_no = text.count("\n", 0, match.start()) + 1
                failures.append(f"{rel_cmake}:{line_no}: missing source-tree file: {token}")

    if failures:
        print("cmake-source-files guard failed:", file=sys.stderr)
        for item in failures:
            print(item, file=sys.stderr)
        return 1

    print("cmake-source-files: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
