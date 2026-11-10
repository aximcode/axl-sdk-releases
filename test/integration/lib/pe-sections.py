#!/usr/bin/env python3
"""pe-sections.py -- read a PE image's COFF header and section table.

For tests that must see what a PE/COFF post-processor changed
(test-pe-set-debug-strtab.sh):

  pe-sections.py coff FILE
      `PTR NSYMS NAME...`: PointerToSymbolTable, NumberOfSymbols and every
      section header's 8-byte name, NUL padding stripped (a long name stays
      as its `/N` string-table reference).
  pe-sections.py sections FILE
      `end OFFSET`, where the last section's raw data ends, then one
      `INDEX SIZE SHA256[:16]` line per section's raw bytes -- except
      .dbgdir, which pe-set-debug patches by design.
  pe-sections.py nameoff FILE NAME
      the file offset of the section header named NAME (for a test that
      plants a bad name there).
  pe-sections.py symptr-off FILE
      the file offset of the COFF header's PointerToSymbolTable field.
  pe-sections.py lastraw FILE
      the last section's PointerToRawData.

Exit 1, with a message, on a file that is not a PE image.
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path


def _layout(blob: bytes) -> tuple[int, int, int]:
    """(COFF header offset, section count, section table offset)."""
    if len(blob) < 0x40 or blob[:2] != b"MZ":
        raise ValueError("not an MZ image")
    coff = struct.unpack_from("<I", blob, 0x3C)[0] + 4
    if blob[coff - 4:coff] != b"PE\0\0":
        raise ValueError("no PE signature")
    count = struct.unpack_from("<H", blob, coff + 2)[0]
    opt_size = struct.unpack_from("<H", blob, coff + 16)[0]
    return coff, count, coff + 20 + opt_size


def coff(blob: bytes) -> str:
    """`PTR NSYMS NAME...` for @a blob."""
    hdr, count, table = _layout(blob)
    ptr, nsyms = struct.unpack_from("<II", blob, hdr + 8)
    names = [blob[table + 40 * i:table + 40 * i + 8].rstrip(b"\0").decode()
             for i in range(count)]
    return " ".join([str(ptr), str(nsyms), *names])


def sections(blob: bytes) -> list[str]:
    """The `end` line, then one line per section but .dbgdir."""
    _, count, table = _layout(blob)
    lines: list[str] = []
    end = 0
    for i in range(count):
        sh = table + 40 * i
        size, off = struct.unpack_from("<II", blob, sh + 16)
        end = max(end, off + size)
        if blob[sh:sh + 8].rstrip(b"\0") == b".dbgdir":
            continue
        digest = hashlib.sha256(blob[off:off + size]).hexdigest()[:16]
        lines.append(f"{i} {size} {digest}")
    return [f"end {end}", *lines]


def nameoff(blob: bytes, name: str) -> int:
    """The file offset of the section header named @a name."""
    _, count, table = _layout(blob)
    for i in range(count):
        if blob[table + 40 * i:table + 40 * i + 8].rstrip(b"\0").decode() == name:
            return table + 40 * i
    raise ValueError(f"no section named {name}")


def lastraw(blob: bytes) -> int:
    """The last section's PointerToRawData."""
    _, count, table = _layout(blob)
    if count == 0:
        raise ValueError("no sections")
    return struct.unpack_from("<I", blob, table + 40 * (count - 1) + 20)[0]


def main(argv: list[str]) -> int:
    modes = {"coff": 3, "sections": 3, "nameoff": 4, "symptr-off": 3, "lastraw": 3}
    if len(argv) < 2 or modes.get(argv[1]) != len(argv):
        print("usage: pe-sections.py coff|sections|symptr-off|lastraw FILE"
              " | nameoff FILE NAME", file=sys.stderr)
        return 2
    try:
        blob = Path(argv[2]).read_bytes()
        if argv[1] == "coff":
            print(coff(blob))
        elif argv[1] == "sections":
            print("\n".join(sections(blob)))
        elif argv[1] == "nameoff":
            print(nameoff(blob, argv[3]))
        elif argv[1] == "symptr-off":
            print(_layout(blob)[0] + 8)
        else:
            print(lastraw(blob))
    except (OSError, ValueError, struct.error) as exc:
        print(f"pe-sections.py: {argv[2]}: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
