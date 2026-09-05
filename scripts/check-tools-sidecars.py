#!/usr/bin/env python3
"""check-tools-sidecars.py -- every share/*.json5 reaches every staging path.

WHY THIS EXISTS.  ``share/`` holds the curated JSON5 databases the shipped
tools auto-discover beside their own ``.efi``: ``pci-ids.json5`` for ``lspci``,
``usb-ids.json5`` for ``lsusb``, ``jedec.json5`` for ``memspd``.  Two places
stage them -- ``.github/workflows/release.yml`` into the UEFI-tools tarball,
``scripts/install.sh`` into an installed SDK -- and each kept its own
hand-maintained list.  The workflow's read ``pci-ids.json5 pci-class.json5``,
written when PCI was the only tool with a database and never revisited when
``lsusb`` and ``memspd`` grew theirs.  So every published
``axl-sdk-uefi-tools-*`` up to and including v4.6.0 carries ``lsusb.efi`` and
``memspd.efi`` and NEITHER of their databases, while the installed SDK carried
all three.  Two lists, one silently a subset, and nothing compared them.

Nothing could see it.  The workflow runs only on a tag and no test reads it,
and the tarball's README described the sidecar that *was* staged -- so the
artifact was internally consistent and still incomplete.

WHAT THIS REQUIRES.  That each staging path reads ``share/`` BY GLOB, in code.
A glob cannot drift: a new sidecar ships with no edit to the staging path.  A
hand-maintained list is rejected outright rather than diffed against ``share/``,
because a list that happens to be complete today is the exact state this gate
exists to stop us returning to.

AND that every sidecar is DOCUMENTED.  ``make-uefi-tools-readme.py`` hard-fails
on a ``share/*.json5`` with no ``SIDECAR_DOCS`` entry -- correct, but that only
fires when a release is cut, and ``verify.sh`` runs no integration test.  So
the coverage check lives here too: adding a sidecar without describing it fails
the local gate rather than the tag build.

WHY IT STRIPS COMMENTS FIRST.  The first version of this gate matched the glob
as a substring of the whole step, and the fix comment in ``release.yml`` -- the
one explaining why a glob is used -- contains the literal ``share/*.json5``.
The gate reported clean against a staging line reverted to a single file.  A
detector satisfied by its own rationale is worth less than none, so the check
runs over CODE lines only and is anchored to an assignment or a for-list.
``scripts/sabotage.sh`` proves it fails; see the control in
``test/integration/test-tools-sidecars-gate.sh``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SHARE = PROJECT_ROOT / "share"
GENERATOR = PROJECT_ROOT / "scripts" / "make-uefi-tools-readme.py"

# Each staging path: the file, and the region of it to read. A workflow step is
# addressed by name; a shell script is read whole.
WORKFLOW = PROJECT_ROOT / ".github" / "workflows" / "release.yml"
INSTALLER = PROJECT_ROOT / "scripts" / "install.sh"
STEP_NAME = "Package tools tarball"

# The glob, in CODE: a shell array assignment or a for-list, not prose. Both
# `_sidecars=("$DIR"/share/*.json5)` and `SIDECARS=(share/*.json5)` match.
GLOB_IN_CODE = re.compile(
    r"""(?:=\(|\bfor\s+\w+\s+in\b)[^\n#]*?share/\*\.json5""",
    re.VERBOSE,
)


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)


def strip_comments(body: str) -> str:
    """Drop whole-line ``#`` comments. Prose must not satisfy a code check."""
    return "\n".join(ln for ln in body.splitlines()
                     if not ln.lstrip().startswith("#"))


def extract_step(text: str, name: str) -> str | None:
    """Return the YAML body of the named workflow step, or None if absent."""
    lines = text.splitlines()
    start = None
    indent = 0
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)-\s+name:\s*(.+?)\s*$", line)
        if m and m.group(2).strip("\"'") == name:
            start = i
            indent = len(m.group(1))
            break
    if start is None:
        return None
    for j in range(start + 1, len(lines)):
        m = re.match(r"^(\s*)-\s+name:\s", lines[j])
        if m and len(m.group(1)) <= indent:
            return "\n".join(lines[start:j])
    return "\n".join(lines[start:])


def documented_sidecars() -> set[str] | None:
    """Filenames with a SIDECAR_DOCS entry, or None if it cannot be read."""
    if not GENERATOR.is_file():
        return None
    text = GENERATOR.read_text()
    m = re.search(r"^SIDECAR_DOCS\s*=\s*\{(.*?)^\}", text,
                  re.DOTALL | re.MULTILINE)
    if not m:
        return None
    return set(re.findall(r'"([^"]+\.json5)"\s*:', m.group(1)))


def main() -> int:
    # "Could not run" and "found nothing" are the same empty set and opposite
    # facts. Every input is checked before anything is reported clean.
    if not SHARE.is_dir():
        fail(f"check-tools-sidecars: FAIL -- {SHARE} not found")
        return 1
    sidecars = sorted(p.name for p in SHARE.glob("*.json5"))
    if not sidecars:
        fail("check-tools-sidecars: FAIL -- share/ holds no *.json5; a gate "
             "with nothing to check passes forever")
        return 1

    errors = 0

    for path, step in ((WORKFLOW, STEP_NAME), (INSTALLER, None)):
        rel = path.relative_to(PROJECT_ROOT)
        if not path.is_file():
            fail(f"check-tools-sidecars: FAIL -- {rel} not found")
            errors += 1
            continue
        body: str | None = path.read_text()
        if step is not None:
            body = extract_step(body or "", step)
            if body is None:
                fail(f"check-tools-sidecars: FAIL -- no '{step}' step in "
                     f"{rel}; the gate cannot see what it is meant to check "
                     "(renamed step?)")
                errors += 1
                continue
        if not GLOB_IN_CODE.search(strip_comments(body)):
            fail(f"check-tools-sidecars: FAIL -- {rel} does not stage "
                 "share/*.json5 by glob in code.")
            fail("  A hand-maintained list is what shipped lsusb.efi and "
                 "memspd.efi with no name database.")
            errors += 1

    undocumented = [s for s in sidecars if s not in (documented_sidecars()
                                                     or set())]
    if documented_sidecars() is None:
        fail("check-tools-sidecars: FAIL -- could not read SIDECAR_DOCS from "
             f"{GENERATOR.relative_to(PROJECT_ROOT)}")
        errors += 1
    elif undocumented:
        fail("check-tools-sidecars: FAIL -- staged but undocumented, so the "
             "tag build would die in make-uefi-tools-readme.py:")
        for u in undocumented:
            fail(f"  share/{u}  (add a SIDECAR_DOCS entry)")
        errors += 1

    if errors:
        print(f"check-tools-sidecars: FAIL -- {errors} problem(s)",
              file=sys.stderr)
        return 1

    print(f"check-tools-sidecars: clean -- {len(sidecars)} sidecar(s), "
          "2 staging path(s) glob-driven, all documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
