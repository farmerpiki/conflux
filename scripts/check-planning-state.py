#!/usr/bin/env python3
"""Cheap guard for proposal/TODO planning state."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROPOSALS = ROOT / "proposals"
ARCHIVE = ROOT / "docs" / "archive"
STATE = ROOT / "todo" / "proposal_state.md"


def fail(message: str) -> None:
    print(f"check-planning-state: {message}", file=sys.stderr)
    raise SystemExit(1)


state_text = STATE.read_text(encoding="utf-8")
if "todo/parallel_priority_plan.md" not in state_text:
    fail("proposal_state.md must name parallel_priority_plan.md as branch-selection partner")

done_branches: set[str] = set()
for line in state_text.splitlines():
    if line.startswith("| DONE |"):
        cells = [cell.strip(" `") for cell in line.strip().strip("|").split("|")]
        if len(cells) >= 2:
            done_branches.add(cells[1])

for path in sorted(PROPOSALS.glob("*.md")):
    rel = path.relative_to(ROOT).as_posix()
    text = path.read_text(encoding="utf-8")
    first = "\n".join(text.splitlines()[:20])
    if "Status:" not in first:
        fail(f"{rel} is active but has no Status: in the first 20 lines")
    if "Branch:" not in first:
        fail(f"{rel} is active but has no Branch: in the first 20 lines")
    if rel not in state_text:
        fail(f"todo/proposal_state.md does not mention active proposal {rel}")

    lowered = first.lower()
    historical = "historical" in first or "archive" in first or "state note" in first
    if (re.search(r"\bimplemented\b", lowered) or re.search(r"\bsuperseded\b", lowered)) and not historical:
        fail(f"{rel} is marked implemented/superseded without a historical/archive state note")

    branch_match = re.search(r"^Branch:\s*`?([^`\n]+)`?", first, re.MULTILINE)
    status_match = re.search(r"^Status:\s*([^\n]+)", first, re.MULTILINE)
    if branch_match and status_match and branch_match.group(1).strip() in done_branches:
        if "in progress" in status_match.group(1).lower():
            fail(f"{rel} claims in progress but proposal_state.md marks its branch DONE")

for path in sorted((ROOT / "todo").glob("*.md")):
    if path.name in {"proposal_state.md", "parallel_priority_plan.md"}:
        continue
    text = path.read_text(encoding="utf-8")
    first = "\n".join(text.splitlines()[:25]).lower()
    if not any(marker in first for marker in ("open", "done reference", "deferred", "archive pointer", "status:")):
        fail(f"{path.relative_to(ROOT).as_posix()} lacks explicit open/done/deferred/archive status near top")

for path in sorted(ARCHIVE.rglob("*.md")):
    if path.name == "README.md":
        continue
    first = "\n".join(path.read_text(encoding="utf-8").splitlines()[:20]).lower()
    if "historical" not in first and "archived" not in first:
        fail(f"{path.relative_to(ROOT).as_posix()} archive file lacks a historical/archive note")

print("check-planning-state: ok")
