#!/usr/bin/env python3
"""Measure first-party Conflux compile/build-time scenarios.

This is intentionally a source-tree harness rather than a benchmark binary: it
measures CMake configure/build cost, include/import smoke targets, quickstart
builds, binary size, and optional timestamp-only incremental rebuilds.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import time
from collections.abc import Sequence


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    target: str
    source: str | None = None
    binary: bool = True


DEFAULT_CASES: tuple[Case, ...] = (
    Case("include_core", "conflux_header_smoke_core"),
    Case("include_json", "conflux_header_smoke_json"),
    Case("include_public_matrix", "conflux_header_smoke_public_includes", binary=False),
    Case("hello_world", "conflux_quickstart_hello", "examples/quickstart/hello.cxx"),
    Case("json_crud", "conflux_quickstart_json_crud", "examples/quickstart/json_crud.cxx"),
    Case("full_http_showcase", "conflux_production_showcase_example", "examples/advanced/production_showcase.cxx"),
    Case("db_basic", "conflux_db_basic", "examples/advanced/db_basic.cxx"),
)


def run(args: Sequence[str], *, cwd: pathlib.Path | None = None) -> tuple[int, str, float]:
    start = time.perf_counter()
    proc = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    elapsed = time.perf_counter() - start
    return proc.returncode, proc.stdout, elapsed


def cmd_line(args: Sequence[str]) -> str:
    return " ".join(subprocess.list2cmdline([arg]) for arg in args)


def tool_version(args: Sequence[str]) -> str:
    exe = shutil.which(args[0])
    if exe is None:
        return "unavailable"
    try:
        out = subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).splitlines()
    except (OSError, subprocess.SubprocessError):
        return "unavailable"
    return out[0] if out else "unavailable"


def configure_args(source: pathlib.Path, build: pathlib.Path, ns: argparse.Namespace) -> list[str]:
    return [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        f"-DCONFLUX_INTERFACE_MODE={ns.interface_mode}",
        f"-DCONFLUX_FEATURE_SET={ns.feature_set}",
        "-DCONFLUX_BUILD_TESTS=OFF",
        "-DCONFLUX_BUILD_BENCHMARKS=OFF",
        "-DCONFLUX_BUILD_EXAMPLES=ON",
    ]


def build_args(build: pathlib.Path, target: str) -> list[str]:
    return ["cmake", "--build", str(build), "--target", target]


def find_binary(build: pathlib.Path, target: str) -> pathlib.Path | None:
    for candidate in build.rglob(target):
        try:
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
        except OSError:
            continue
    return None


def touch_for_incremental(source: pathlib.Path, rel: str) -> tuple[pathlib.Path, int, int] | None:
    path = source / rel
    try:
        st = path.stat()
    except OSError:
        return None
    now_ns = time.time_ns()
    os.utime(path, ns=(now_ns, now_ns))
    return path, st.st_atime_ns, st.st_mtime_ns


def restore_timestamp(saved: tuple[pathlib.Path, int, int] | None) -> None:
    if saved is None:
        return
    path, atime_ns, mtime_ns = saved
    try:
        os.utime(path, ns=(atime_ns, mtime_ns))
    except OSError:
        pass


def emit_json(data: dict, pretty: bool) -> None:
    if pretty:
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        print(json.dumps(data, sort_keys=True))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path("build/compile-time-bench"))
    parser.add_argument("--feature-set", default="http-minimal")
    parser.add_argument("--interface-mode", choices=("HEADER_INTERFACE", "MODULE_INTERFACE"), default="HEADER_INTERFACE")
    parser.add_argument("--target", action="append", help="Measure only this target; may be repeated")
    parser.add_argument("--no-clean", action="store_true", help="Reuse existing build directory")
    parser.add_argument("--incremental", action="store_true", help="Also touch known case sources and measure rebuild cost")
    parser.add_argument("--keep-going", action="store_true", help="Record failed/absent optional targets instead of exiting")
    parser.add_argument("--pretty", action="store_true")
    ns = parser.parse_args(argv)

    source = ns.source.resolve()
    build = ns.build.resolve()
    cases = [c for c in DEFAULT_CASES if ns.target is None or c.target in ns.target or c.name in ns.target]
    selected_targets = {c.target for c in cases}
    for target in ns.target or []:
        if target not in selected_targets and all(target != c.name for c in cases):
            cases.append(Case(target, target))

    if not ns.no_clean and build.exists():
        shutil.rmtree(build)
    build.mkdir(parents=True, exist_ok=True)

    output: dict = {
        "schema": "conflux.compile_time_bench.v1",
        "source": str(source),
        "build": str(build),
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version.split()[0],
            "cmake": tool_version(["cmake", "--version"]),
            "cxx": os.environ.get("CXX", "default"),
        },
        "config": {
            "feature_set": ns.feature_set,
            "interface_mode": ns.interface_mode,
            "incremental": ns.incremental,
        },
        "commands": {},
        "configure": {},
        "cases": [],
    }

    cfg_cmd = configure_args(source, build, ns)
    output["commands"]["configure"] = cmd_line(cfg_cmd)
    cfg_rc, cfg_out, cfg_sec = run(cfg_cmd)
    output["configure"] = {"ok": cfg_rc == 0, "seconds": cfg_sec, "returncode": cfg_rc}
    if cfg_rc != 0:
        output["configure"]["output_tail"] = cfg_out[-4000:]
        emit_json(output, ns.pretty)
        return cfg_rc

    overall_rc = 0
    for case in cases:
        row: dict = {"name": case.name, "target": case.target}
        cmd = build_args(build, case.target)
        row["command"] = cmd_line(cmd)
        rc, out, sec = run(cmd)
        row["clean_build"] = {"ok": rc == 0, "seconds": sec, "returncode": rc}
        if rc != 0:
            row["clean_build"]["output_tail"] = out[-4000:]
            overall_rc = rc if overall_rc == 0 else overall_rc
            output["cases"].append(row)
            if not ns.keep_going:
                break
            continue

        if case.binary:
            binary = find_binary(build, case.target)
            if binary is not None:
                row["binary"] = {"path": str(binary), "bytes": binary.stat().st_size}

        if ns.incremental and case.source is not None:
            saved = touch_for_incremental(source, case.source)
            try:
                inc_rc, inc_out, inc_sec = run(cmd)
            finally:
                restore_timestamp(saved)
            row["incremental_build"] = {"ok": inc_rc == 0, "seconds": inc_sec, "returncode": inc_rc, "source": case.source}
            if inc_rc != 0:
                row["incremental_build"]["output_tail"] = inc_out[-4000:]
                overall_rc = inc_rc if overall_rc == 0 else overall_rc
                if not ns.keep_going:
                    output["cases"].append(row)
                    break

        output["cases"].append(row)

    emit_json(output, ns.pretty)
    return overall_rc


if __name__ == "__main__":
    raise SystemExit(main())
