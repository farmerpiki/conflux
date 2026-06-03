#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys


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
    "http": ["template", "pg", "db"],
}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("component")
    parser.add_argument("--extra", action="append", default=[])
    args = parser.parse_args(argv[1:])
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
