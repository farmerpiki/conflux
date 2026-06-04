#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from component_registry import exports

ROOT = Path(__file__).resolve().parents[1]
PACKAGE_ALIASES = {"curated", "extended", "complete", "db"}


POLICIES = {
    "core": ["http", "http1", "http2", "http3", "http_protocol", "template", "pg", "db"],
    "json": [
        "http",
        "http1",
        "http2",
        "http3",
        "http_protocol",
        "http_compression",
        "net_tls",
        "template",
        "pg",
        "db",
        "dns",
        "work",
    ],
    "template": ["http", "http1", "http2", "http3", "http_protocol", "pg", "db", "dns"],
    "dns": ["http", "http1", "http2", "http3", "http_protocol", "template", "pg", "db"],
    "pg": ["http", "http1", "http2", "http3", "http_protocol", "template", "db"],
    "http": ["http_compression", "net_tls", "template", "pg", "db"],
}


def public_components() -> set[str]:
    return exports(ROOT, "REQUESTABLE", "EXPLICIT", "EXPERIMENTAL")


def validate_policy(component: str, components: list[str], known_components: set[str]) -> list[str]:
    errors: list[str] = []
    if component not in known_components:
        errors.append(f"unknown package smoke forbidden component policy: {component}")
    seen: set[str] = set()
    for item in components:
        if item in seen:
            errors.append(f"{component}: duplicate forbidden component {item}")
        seen.add(item)
        if item not in known_components:
            errors.append(f"{component}: unknown forbidden component {item}")
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("component")
    parser.add_argument("--extra", action="append", default=[])
    args = parser.parse_args(argv[1:])
    known_components = public_components() | PACKAGE_ALIASES
    errors = [
        error
        for component, components in sorted(POLICIES.items())
        for error in validate_policy(component, components, known_components)
    ]
    unknown_extra = [component for component in args.extra if component not in known_components]
    errors.extend(f"unknown extra forbidden component {component}" for component in unknown_extra)
    if errors:
        print("package-smoke-forbidden-components: invalid policy", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    try:
        components = [*POLICIES[args.component]]
    except KeyError:
        print(f"package-smoke-forbidden-components: unknown component policy: {args.component}", file=sys.stderr)
        return 1
    seen: set[str] = set()
    ordered: list[str] = []
    for component in [*components, *args.extra]:
        if component and component not in seen:
            seen.add(component)
            ordered.append(component)
    print(";".join(ordered))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
