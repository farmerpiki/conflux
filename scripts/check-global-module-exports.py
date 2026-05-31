#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

EXPORT_RE = re.compile(r"^\s*export\b")
EXPORT_DECL_RE = re.compile(
    r"^\s*export\s+(class|struct|enum|template|using|typedef|concept|constexpr|consteval|constinit|inline|"
    r"auto|void|bool|char|short|int|long|float|double|std::|[A-Za-z_][\w:<>,\s*&]+)\b"
)
NAMESPACE_RE = re.compile(r"\b(?:export\s+)?namespace\s+([A-Za-z_][\w:]*)?\s*(?=[{=])")


def scrub(line: str, in_block_comment: bool) -> tuple[str, bool]:
    out: list[str] = []
    i = 0
    quote = ""
    while i < len(line):
        ch = line[i]
        nxt = line[i + 1] if i + 1 < len(line) else ""
        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
            else:
                i += 1
            out.append(" ")
            continue
        if quote:
            if ch == "\\":
                out.extend("  ")
                i += 2
                continue
            if ch == quote:
                quote = ""
            out.append(" ")
            i += 1
            continue
        if ch == "/" and nxt == "*":
            in_block_comment = True
            out.extend("  ")
            i += 2
            continue
        if ch == "/" and nxt == "/":
            out.extend(" " * (len(line) - i))
            break
        if ch in {'"', "'"}:
            quote = ch
            out.append(" ")
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out), in_block_comment


def namespace_events(line: str) -> list[tuple[int, str]]:
    return [(match.start(), match.group(1) or "") for match in NAMESPACE_RE.finditer(line)]


def check_file(path: Path) -> list[str]:
    failures: list[str] = []
    depth = 0
    ns_stack: list[tuple[str, int]] = []
    pending = namespace_events("")
    in_block_comment = False

    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line, in_block_comment = scrub(raw, in_block_comment)
        pending = namespace_events(line)
        pending_idx = 0

        stripped = line.strip()
        if EXPORT_RE.match(stripped):
            if stripped.startswith("export module ") or stripped.startswith("export import "):
                pass
            elif stripped.startswith("export namespace "):
                name = stripped.removeprefix("export namespace ").split("{", 1)[0].strip()
                in_conflux = any(stack_name == "conflux" or stack_name.startswith("conflux::") for stack_name, _ in ns_stack)
                if not in_conflux and not name.startswith("conflux"):
                    failures.append(f"{path.relative_to(ROOT)}:{line_no}: exported namespace `{name}` is not under conflux")
            elif EXPORT_DECL_RE.match(line):
                in_conflux = any(name == "conflux" or name.startswith("conflux::") for name, _ in ns_stack)
                if not in_conflux:
                    failures.append(f"{path.relative_to(ROOT)}:{line_no}: global exported declaration: {raw.strip()}")

        for pos, ch in enumerate(line):
            if ch == "{":
                depth += 1
                while pending_idx < len(pending) and pending[pending_idx][0] < pos:
                    name = pending[pending_idx][1]
                    if name:
                        ns_stack.append((name, depth))
                    pending_idx += 1
                    break
            elif ch == "}":
                depth -= 1
                while ns_stack and ns_stack[-1][1] > depth:
                    ns_stack.pop()
                if depth < 0:
                    depth = 0

    return failures


def main() -> int:
    failures: list[str] = []
    for path in sorted(SRC.rglob("*.cxx")):
        failures.extend(check_file(path))
    if failures:
        print("global module export guard failed:", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    print("global module exports: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
