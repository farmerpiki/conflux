#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SNAPSHOTS = [
    ROOT / "tests" / "http_facade_api_snapshot.cxx",
    ROOT / "tests" / "http_facade_extended_api_snapshot.cxx",
]
ALLOWED_IMPORTS = {
    "std",
    "conflux.http",
    "conflux.http.extended",
}
FORBIDDEN_IMPORT_PREFIXES = (
    "conflux.net.",
    "conflux.uring",
    "conflux.socket_io",
    "conflux.file_io",
)
IMPORT_RE = re.compile(r"^\s*import\s+([A-Za-z0-9_.:]+)\s*;", re.MULTILINE)


def main() -> int:
    failures: list[str] = []
    for path in SNAPSHOTS:
        text = path.read_text(encoding="utf-8")
        for module in IMPORT_RE.findall(text):
            if module in ALLOWED_IMPORTS:
                continue
            label = "internal module" if module.startswith(FORBIDDEN_IMPORT_PREFIXES) else "non-facade module"
            failures.append(
                f"{path.relative_to(ROOT)} imports {label} `{module}`; "
                "HTTP facade snapshots must exercise only facade imports"
            )

    if failures:
        print("http-facade snapshot guard failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("http-facade snapshot guard: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
