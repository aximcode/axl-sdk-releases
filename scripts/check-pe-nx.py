#!/usr/bin/env python3
"""check-pe-nx.py — assert a PE32+ EFI image declares NX-compatibility.

Secure Boot is normally paired with firmware memory-protection (W^X / NX)
enforcement. axl-sdk's images are built W^X-clean (R-X .text, RW .data), so the
PE optional header's DllCharacteristics must advertise the NX_COMPAT bit
(0x0100) — otherwise protected firmware may warn on or refuse the image. This
checker reads that one bit, independently of pe-set-debug (the tool that sets
it), so the build can prove the bit landed.

Usage: check-pe-nx.py <file.efi> [<file.efi> ...]
Exit: 0 if every image has NX_COMPAT set; 1 otherwise.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

MZ_MAGIC = 0x5A4D
PE_MAGIC = 0x00004550
PE32PLUS_MAGIC = 0x20B
# DllCharacteristics offset within the PE32+ optional header.
OPT_DLLCHARACTERISTICS_OFF = 70
IMAGE_DLLCHARACTERISTICS_NX_COMPAT = 0x0100


def nx_compat_set(path: Path) -> bool:
    """Return True if @path is a PE32+ image with the NX_COMPAT bit set."""
    data = path.read_bytes()
    if len(data) < 64 or struct.unpack_from("<H", data, 0)[0] != MZ_MAGIC:
        raise ValueError(f"{path}: not a PE file (bad MZ magic)")
    pe_off = struct.unpack_from("<I", data, 60)[0]
    if struct.unpack_from("<I", data, pe_off)[0] != PE_MAGIC:
        raise ValueError(f"{path}: bad PE signature")
    opt = pe_off + 4 + 20  # skip PE sig (4) + COFF header (20)
    if struct.unpack_from("<H", data, opt)[0] != PE32PLUS_MAGIC:
        raise ValueError(f"{path}: not PE32+")
    dllchar = struct.unpack_from("<H", data, opt + OPT_DLLCHARACTERISTICS_OFF)[0]
    return bool(dllchar & IMAGE_DLLCHARACTERISTICS_NX_COMPAT)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    failed = 0
    for arg in argv[1:]:
        path = Path(arg)
        try:
            if nx_compat_set(path):
                print(f"  NX_COMPAT set: {path.name}")
            else:
                print(f"  MISSING NX_COMPAT: {path.name}", file=sys.stderr)
                failed += 1
        except (ValueError, OSError) as exc:
            print(f"  ERROR: {exc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check-pe-nx: {failed} image(s) missing NX_COMPAT", file=sys.stderr)
        return 1
    print(f"check-pe-nx: clean ({len(argv) - 1} image(s) NX-compatible)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
