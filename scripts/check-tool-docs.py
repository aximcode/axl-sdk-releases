#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""Every shipped UEFI tool has a row in README.md's tool table.

README.md IS the releases-site README -- `aximcode/axl-sdk-releases`
serves a byte-identical copy -- so a tool missing here is a tool missing
from the public front page. `lsacpi` shipped in 4.3.3 and was absent for
three releases; the census that found it found sixteen more.

This does NOT generate the table, and that is deliberate. The rows are
hand-written prose up to ~320 characters describing real flags and
behaviour, while the only machine-readable descriptions available
(`devkit.conf`'s `desc:` lines) are ~40-character console labels for an
on-screen menu. Generating from those would replace documentation with
labels. What is checkable is COMPLETENESS, so that is what is checked.

Three ways to be wrong, all covered:
  - a shipped tool with no row (the drift that happened);
  - a row for a tool that no longer ships (the reverse drift);
  - two rows for one tool (happened while adding the sixteen).

THE TARBALL'S OWN README is the other prose list, and it drifted further --
13 of 38. It goes the opposite way for the reason above inverted: its entries
ARE ~40-character labels, so `devkit.conf` fits it exactly and
`scripts/make-uefi-tools-readme.py` derives it. Nothing here can check that
file, because it exists only inside a release artifact; what IS checkable is
that `release.yml` still delegates to the generator rather than growing the
hand-written heredoc back.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
WORKFLOW = ROOT / ".github/workflows/release.yml"
GENERATOR = "make-uefi-tools-readme.py"

# Built and shipped, but deliberately absent from a table of user tools.
# Each needs a reason, not just an entry.
NOT_USER_TOOLS = {
    "crashtest":  "a deliberate-fault fixture for exercising the crash handler",
    "fbcon":      "a console app that carries an embedded driver, not a utility",
    "kbtune-drv": "a driver, not a command",
}


def main() -> int:
    def make_var(name: str) -> list[str]:
        r = subprocess.run(["make", "-s", f"print-{name}"], cwd=ROOT,
                           capture_output=True, text=True, check=False)
        if r.returncode != 0:
            print(f"check-tool-docs: could not read {name} from the Makefile")
            sys.exit(1)
        return r.stdout.split()

    shipped = set(make_var("TOOL_NAMES")) | set(make_var("TOOL_EXTRA_APPS"))
    if not shipped:
        print("check-tool-docs: the tool list came back EMPTY -- this gate "
              "would pass vacuously")
        return 1

    rows = re.findall(r"^\| `([a-z0-9-]+)`", README.read_text(encoding="utf-8"),
                      re.M)
    documented = set(rows)
    errors = 0

    for dup in sorted({r for r in rows if rows.count(r) > 1}):
        print(f"check-tool-docs: `{dup}` has {rows.count(dup)} rows in "
              f"README.md's table")
        errors += 1

    for tool in sorted(shipped - documented - set(NOT_USER_TOOLS)):
        print(f"check-tool-docs: `{tool}` ships but has no row in README.md's "
              f"tool table (which is also the releases-site front page)")
        errors += 1

    for tool in sorted(documented - shipped):
        print(f"check-tool-docs: README.md documents `{tool}`, which the "
              f"Makefile no longer builds")
        errors += 1

    for tool in sorted(set(NOT_USER_TOOLS) & documented):
        print(f"check-tool-docs: `{tool}` is listed as not-a-user-tool "
              f"({NOT_USER_TOOLS[tool]}) but README.md documents it -- pick one")
        errors += 1

    if GENERATOR not in WORKFLOW.read_text(encoding="utf-8"):
        print(f"check-tool-docs: release.yml no longer calls {GENERATOR} -- the "
              f"tarball's README is back to a hand-written list, which is how "
              f"it came to name 13 of {len(shipped)} tools")
        errors += 1

    if errors:
        print(f"check-tool-docs: {errors} problem(s)")
        return 1
    print(f"check-tool-docs: clean -- {len(documented)} documented, "
          f"{len(NOT_USER_TOOLS)} excluded with a reason, tarball README derived")
    return 0


if __name__ == "__main__":
    sys.exit(main())
