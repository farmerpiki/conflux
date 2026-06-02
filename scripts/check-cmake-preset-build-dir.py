#!/usr/bin/env python3
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts" / "cmake-preset-build-dir.py"


def fail(message: str) -> None:
    print(f"check-cmake-preset-build-dir: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(source_dir: Path, preset: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(HELPER), str(source_dir), preset],
        check=False,
        text=True,
        capture_output=True,
    )


def write(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="conflux-preset-build-dir-") as tmp:
        source = Path(tmp) / "project"
        source.mkdir()
        write(
            source / "base.json",
            """
{
  "version": 10,
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "binaryDir": "${sourceParentDir}/${sourceDirName}-${presetName}"
    }
  ]
}
""",
        )
        write(
            source / "CMakePresets.json",
            """
{
  "version": 10,
  "include": ["base.json"],
  "configurePresets": [
    {
      "name": "derived",
      "inherits": "base"
    },
    {
      "name": "direct",
      "binaryDir": "${sourceDir}/out/${presetName}"
    }
  ]
}
""",
        )

        derived = run(source, "derived")
        expected = f"{source.parent}/project-derived"
        if derived.returncode != 0:
            fail(derived.stderr.strip() or derived.stdout.strip())
        if derived.stdout.strip() != expected:
            fail(f"expected included inherited binaryDir {expected}, got {derived.stdout.strip()}")

        direct = run(source, "direct")
        expected = f"{source}/out/direct"
        if direct.returncode != 0:
            fail(direct.stderr.strip() or direct.stdout.strip())
        if direct.stdout.strip() != expected:
            fail(f"expected direct binaryDir {expected}, got {direct.stdout.strip()}")

        write(
            source / "CMakePresets.json",
            """
{
  "version": 10,
  "include": ["base.json"],
  "configurePresets": [
    {
      "name": "base",
      "binaryDir": "${sourceDir}/duplicate"
    }
  ]
}
""",
        )
        duplicate = run(source, "base")
        if duplicate.returncode == 0:
            fail("duplicate configure preset names must be rejected")
        if "duplicate configure preset: base" not in duplicate.stderr:
            fail(f"unexpected duplicate-preset diagnostic: {duplicate.stderr.strip()}")

        write(
            source / "CMakePresets.json",
            """
{
  "version": 10,
  "configurePresets": [
    {
      "name": "one",
      "inherits": "two"
    },
    {
      "name": "two",
      "inherits": "one"
    }
  ]
}
""",
        )
        inheritance_cycle = run(source, "one")
        if inheritance_cycle.returncode == 0:
            fail("cyclic preset inheritance must be rejected")
        if "cyclic preset inheritance involving one" not in inheritance_cycle.stderr:
            fail(f"unexpected inheritance-cycle diagnostic: {inheritance_cycle.stderr.strip()}")

        write(
            source / "base.json",
            """
{
  "version": 10,
  "include": ["CMakePresets.json"],
  "configurePresets": []
}
""",
        )
        write(
            source / "CMakePresets.json",
            """
{
  "version": 10,
  "include": ["base.json"],
  "configurePresets": []
}
""",
        )
        include_cycle = run(source, "missing")
        if include_cycle.returncode == 0:
            fail("cyclic preset includes must be rejected")
        if "cyclic preset include involving" not in include_cycle.stderr:
            fail(f"unexpected include-cycle diagnostic: {include_cycle.stderr.strip()}")

    print("check-cmake-preset-build-dir: ok")


if __name__ == "__main__":
    main()
