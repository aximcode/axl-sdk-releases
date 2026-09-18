#!/usr/bin/env python3
"""check-no-inner-map-refresh.py — keep the shell's `map -r` out of RAM-disk code.

`axl_map_refresh()` runs the UEFI Shell's `map -r`, and that command is not a
local operation on the caller's own volume:

  * it first calls ProbeForMediaChange() — a dummy ReadBlocks against every
    physical block device (logical partitions are skipped) that lets each one
    REPORT a media change. A device that does has its BlockIo re-installed,
    tearing down the partition/filesystem stack above it and invalidating
    handles other software still holds (a running startup.nsh, an open editor,
    a virtual-media volume);
  * ShellCommandCreateInitialMappingsAndPaths() then empties the WHOLE map list
    — every user-made name and each mapping's remembered current directory —
    and blanks %path%. On the modern shell EFI_SHELL_PROTOCOL.Execute usually
    runs a nested Shell.efi image, so that half hits only the nested instance
    and the damage is invisible in a QEMU test — but a shell started `-nonest`
    runs it in-context, and the old EFI 1.x shell's SHELL_ENVIRONMENT.Execute
    always does, rebuilding the real map.

`mkrd` used to run it from inside create/destroy. That is the mechanism which
matches a field report (an editor's "Read File Failed", truncated small files, a
user map name needing a remap); the causation was never isolated on hardware.
Either way, naming a disk is the caller's business: mkrd assigns names with
SetMap (no global side effects), and whoever wants the shell's map rebuilt runs
`map -r` themselves.

A behavioral test cannot guard this on a nesting shell (the default) — the
nested Execute hides it — so this guard is source-level. axl_map_refresh()
itself stays: it is public API, for a caller that asks for a map refresh on
purpose. What it forbids is a RAM-disk create or destroy deciding to run one
on the caller's behalf.

Usage: check-no-inner-map-refresh.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Everything that creates or destroys a RAM disk. Globs, not fixed names, so a
# new file in the module (or a second RAM-disk tool) cannot slip past the gate.
GUARDED_GLOBS: tuple[str, ...] = (
    "src/ramdisk/*.c",
    "tools/mkrd*.c",
)

CALL = re.compile(r"\baxl_map_refresh\s*\(")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    """Blank out comments, keeping line numbering intact.

    Both guarded files are comment-dense and explain at length why they do NOT
    call axl_map_refresh(), so a naive scan would flag its own rationale. Each
    comment becomes the same number of newlines it spanned.

    Blunt on purpose: a `//` or `/*` inside a string literal is treated as a
    comment, so a call hidden after one on the same line would be missed. The
    realistic reintroduction is a bare statement, which is caught.
    """

    def blank(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    return LINE_COMMENT.sub("", BLOCK_COMMENT.sub(blank, text))


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    failures: list[str] = []
    scanned = 0

    for glob in GUARDED_GLOBS:
        paths = sorted(root.glob(glob))
        if not paths:
            failures.append(f"{glob}: matched no files -- has the module moved?")
            continue
        for path in paths:
            scanned += 1
            rel = path.relative_to(root)
            code = strip_comments(path.read_text(encoding="utf-8"))
            for lineno, line in enumerate(code.splitlines(), start=1):
                if CALL.search(line):
                    failures.append(f"{rel}:{lineno}: calls axl_map_refresh()")

    if failures:
        print("check-no-inner-map-refresh: FAIL")
        for line in failures:
            print(f"  {line}")
        print(
            "  Creating or destroying a RAM disk must not run the shell's `map -r`:\n"
            "  it probes every physical block device, and any that reports a media\n"
            "  change has its BlockIo re-installed under handles other software\n"
            "  still holds; it then rebuilds the shell's map list.\n"
            "  Name the disk with SetMap, and tell the user to run `map -r`."
        )
        return 1

    print(f"check-no-inner-map-refresh: ok ({scanned} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
