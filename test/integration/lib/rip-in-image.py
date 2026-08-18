#!/usr/bin/env python3
"""rip-in-image.py -- does the crash report's faulting PC land in a loaded image?

Reads a CrashHandler crash report on stdin and exits 0 when the faulting
instruction pointer falls inside one of the images the report itself lists.

This is the assertion that separates a real capture from a mis-decoded one.
`include/uefi/generated/cpu-arch.h` once declared `void *FxSaveState` where
UEFI 2.11 has an inline 512-byte `EFI_FX_SAVE_STATE_X64`, so every field after
it was read 504 bytes early: the report carried RIP=0x4F307F9B6302D008 with
every GPR zero. Both the live print and the persisted record agreed on that
value, so comparing them to each other could not notice -- only asking whether
the address is CODE can.

Usage: report_text | rip-in-image.py
"""

from __future__ import annotations

import re
import sys

# RIP on x64, ELR on aa64 -- the report labels the faulting PC per arch.
PC_RE = re.compile(r"^ *(?:RIP|ELR)=([0-9A-Fa-f]+)", re.M)
IMAGE_RE = re.compile(r"^  0x([0-9A-Fa-f]{16}) 0x([0-9A-Fa-f]+)", re.M)


def main() -> int:
    text = sys.stdin.read().replace("\r", "")

    rip_match = PC_RE.search(text)
    if rip_match is None:
        print("rip-in-image: no RIP=/ELR= line in report", file=sys.stderr)
        return 1
    rip = int(rip_match.group(1), 16)

    images = [(int(base, 16), int(size, 16))
              for base, size in IMAGE_RE.findall(text)]
    if not images:
        print("rip-in-image: no loaded-image table in report", file=sys.stderr)
        return 1

    for base, size in images:
        if base <= rip < base + size:
            return 0

    print(f"rip-in-image: RIP 0x{rip:016X} is outside all {len(images)} "
          f"listed images", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
