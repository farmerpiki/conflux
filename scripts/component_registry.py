from __future__ import annotations

import re
from pathlib import Path
from typing import NamedTuple


class ComponentDeclaration(NamedTuple):
    target: str
    export: str
    kind: str
    tier: str


COMPONENT_RE = re.compile(
    r'"([^"|]+)\|([^"|]+)\|(REQUESTABLE|EXPLICIT|EXPERIMENTAL|SUPPORT)\|'
    r'(STABLE|ADVANCED|EXPERIMENTAL|INTERNAL_SUPPORT)"'
)


def declarations(root: Path) -> list[ComponentDeclaration]:
    text = (root / "cmake" / "ConfluxComponentRegistry.cmake").read_text(encoding="utf-8")
    items = [
        ComponentDeclaration(target, export, kind, tier)
        for target, export, kind, tier in COMPONENT_RE.findall(text)
    ]
    if not items:
        raise ValueError("missing component declarations")
    return items


def by_kind(root: Path, kind: str) -> dict[str, tuple[str, str]]:
    components: dict[str, tuple[str, str]] = {}
    for declaration in declarations(root):
        if declaration.kind != kind:
            continue
        if declaration.export in components:
            raise ValueError(f"duplicate CMake component export name: {declaration.export}")
        components[declaration.export] = (declaration.target, declaration.tier)
    if not components:
        raise ValueError(f"missing {kind} component declarations")
    return components


def exports(root: Path, *kinds: str) -> set[str]:
    wanted = set(kinds)
    return {declaration.export for declaration in declarations(root) if declaration.kind in wanted}
