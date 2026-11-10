#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""Cut the EFI driver image out of a PCI option ROM file.

A PCI expansion ROM is a chain of images, each starting 0x55 0xAA with a PCI
data structure ("PCIR") naming its code type and length; an EFI image (code
type 3) carries a header naming its machine type, whether its payload is
compressed (1 = the UEFI compression algorithm), and where the payload starts.
QEMU's efi-<nic>.rom is a legacy PXE image followed by such an EFI image.

    efi-rom-image.py ROM MACHINE OUT

writes the payload of the first EFI image for MACHINE (hex, e.g. 0x8664) to
OUT and prints "compressed" or "raw". A compressed payload is a UEFI-compressed
PE that `TianoCompress -d --uefi` expands. Exit 1, saying why, when the ROM has
no such image or is malformed.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

ROM_SIG = b"\x55\xaa"
PCIR_PTR_OFF = 0x18
EFI_SIG = 0x0EF1
CODE_TYPE_EFI = 3
LAST_IMAGE = 0x80


def find_efi_payload(rom: bytes, machine: int) -> tuple[bytes, bool]:
    """The payload of the first EFI image for @machine, and whether it is compressed."""
    off = 0
    while off + 0x1A <= len(rom):
        if rom[off:off + 2] != ROM_SIG:
            raise ValueError(f"no ROM signature at offset {off:#x}")
        pcir = off + struct.unpack_from("<H", rom, off + PCIR_PTR_OFF)[0]
        if rom[pcir:pcir + 4] != b"PCIR":
            raise ValueError(f"no PCIR structure at offset {pcir:#x}")
        length = struct.unpack_from("<H", rom, pcir + 0x10)[0] * 512
        code_type = rom[pcir + 0x14]
        indicator = rom[pcir + 0x15]
        if length == 0:
            raise ValueError(f"zero-length image at offset {off:#x}")
        if code_type == CODE_TYPE_EFI:
            sig, _subsys, mach, comp = struct.unpack_from("<IHHH", rom, off + 4)
            payload_off = struct.unpack_from("<H", rom, off + 0x16)[0]
            if sig == EFI_SIG and mach == machine:
                return rom[off + payload_off:off + length], comp == 1
        if indicator & LAST_IMAGE:
            break
        off += length
    raise ValueError(f"no EFI image for machine {machine:#x}")


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(f"usage: {argv[0]} ROM MACHINE OUT", file=sys.stderr)
        return 2
    rom = Path(argv[1]).read_bytes()
    try:
        payload, compressed = find_efi_payload(rom, int(argv[2], 16))
    except ValueError as e:
        print(f"{argv[1]}: {e}", file=sys.stderr)
        return 1
    Path(argv[3]).write_bytes(payload)
    print("compressed" if compressed else "raw")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
