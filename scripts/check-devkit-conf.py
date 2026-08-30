#!/usr/bin/env python3
"""check-devkit-conf -- every shipped tool reaches the devkit image.

WHY THIS EXISTS. `lsacpi` shipped on 2026-08-28 (four phases, its own spec and
plan), went into `TOOL_NAMES`, and reached the tools tarball and the `axl`
busybox automatically because both DERIVE from that variable -- the tarball
copies `tools/*.efi` and BUSYBOX_OBJS is a patsubst over TOOL_NAMES. It did
NOT reach uefi-devkit's bootable image, because `devkit.conf` is a hand-written
list and nobody added a line to it. Nothing noticed: no test reads that file,
so the only reference to it outside the repo is the consumer that silently got
34 tools minus one.

The same afternoon showed the identical drift in release.yml's tarball
sanity list, which had missed SEVEN tools (axbench, cut, kbtune, lsacpi,
lsproto, netload, tr) while its own comment said "keep this list in sync with
TOOL_NAMES". That one was fixed by deriving it. This file cannot be derived --
it carries per-tool display names and descriptions that only a human can
write -- so it gets a gate instead.

WHAT IT DOES NOT POLICE. Entries with no TOOL_NAMES counterpart are FINE and
expected: the devkit image also stages drivers and apps that are deliberately
not busybox verbs (the Makefile says so at each one -- crashtest, fbcon, 9p).
Flagging those would make the gate cry wolf on a correct file, and a gate that
cries wolf gets its output ignored.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONF = REPO / "devkit.conf"


def tool_names() -> list[str]:
    """The canonical tool set, asked of the Makefile rather than restated."""
    out = subprocess.run(
        ["make", "-s", "print-TOOL_NAMES"],
        cwd=REPO, capture_output=True, text=True, check=False,
    )
    # A gate that cannot RUN its source of truth must say so, not report clean.
    # "The tool could not start" and "the tool found nothing" are the same
    # empty string and opposite facts.
    if out.returncode != 0:
        print("check-devkit-conf: FAIL -- `make -s print-TOOL_NAMES` exited "
              f"{out.returncode}; cannot tell which tools should be present.",
              file=sys.stderr)
        print(out.stderr.strip()[:400], file=sys.stderr)
        sys.exit(1)
    names = out.stdout.split()
    if len(names) < 25:
        print(f"check-devkit-conf: FAIL -- print-TOOL_NAMES named only "
              f"{len(names)} tools. A gate checking almost nothing passes "
              "forever.", file=sys.stderr)
        sys.exit(1)
    return names


def main() -> int:
    if not CONF.is_file():
        print(f"check-devkit-conf: FAIL -- {CONF} not found", file=sys.stderr)
        return 1
    text = CONF.read_text()

    staged = set(re.findall(
        r"^binary\s+\S*/tools/([A-Za-z0-9_-]+)\.efi", text, re.M))
    described = {m.lower() for m in re.findall(r"^desc:\s+(\S+)", text, re.M)}

    missing_binary = [t for t in tool_names() if t not in staged]
    # A tool staged but not described gets no startup.nsh listing or alias, so
    # it is on the image and invisible -- half-delivered rather than delivered.
    missing_desc = [t for t in tool_names()
                    if t in staged and t.lower() not in described]

    if missing_binary or missing_desc:
        print("check-devkit-conf: FAIL", file=sys.stderr)
        for t in missing_binary:
            print(f"  {t}: in TOOL_NAMES, no `binary` line in devkit.conf -- "
                  "it will not reach the devkit image at all", file=sys.stderr)
        for t in missing_desc:
            print(f"  {t}: staged but has no `desc:` line -- it lands on the "
                  "image with no startup.nsh listing or alias",
                  file=sys.stderr)
        print("\n  devkit.conf is a hand-written list; TOOL_NAMES is the "
              "canonical set. Add the missing line(s).", file=sys.stderr)
        return 1

    print(f"check-devkit-conf: clean -- {len(tool_names())} tools staged and "
          "described in devkit.conf.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
