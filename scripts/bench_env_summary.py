#!/usr/bin/env python3
"""Print benchmark host metadata without changing host settings."""

from __future__ import annotations

import pathlib
import platform
import shutil
import subprocess


def cmd(args: list[str]) -> str:
    exe = shutil.which(args[0])
    if exe is None:
        return "unavailable"
    try:
        return subprocess.check_output([exe, *args[1:]], text=True, stderr=subprocess.DEVNULL).strip()
    except subprocess.SubprocessError:
        return "unavailable"


def first_line(path: str) -> str:
    try:
        return pathlib.Path(path).read_text(encoding="utf-8").splitlines()[0]
    except (OSError, IndexError, UnicodeDecodeError):
        return "unavailable"


def main() -> int:
    microcode = cmd(["bash", "-lc", "grep -m1 '^microcode' /proc/cpuinfo | cut -d: -f2-"])
    print(f"kernel={platform.platform()}")
    print(f"machine={platform.machine()}")
    print(f"cpu_model={first_line('/proc/cpuinfo')}")
    print(f"microcode={microcode}")
    print(f"g++={cmd(['g++', '--version']).splitlines()[0]}")
    print(f"clang++={cmd(['clang++', '--version']).splitlines()[0]}")
    print(f"cmake={cmd(['cmake', '--version']).splitlines()[0]}")
    print(f"git_commit={cmd(['git', 'rev-parse', 'HEAD'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
