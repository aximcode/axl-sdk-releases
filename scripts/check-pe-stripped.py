#!/usr/bin/env python3
"""check-pe-stripped.py — a produced .efi carries no COFF symbol table.

WHY. `objcopy`'s ELF -> PE/COFF conversion writes a COFF symbol table and its
string table into the image, after the last section. The firmware never reads
either: the PE loader uses the section table, the relocation directory and the
entry point, and nothing else. Measured on a do-nothing AXL app it was **9,989
bytes -- 21.1% of a 47,365-byte image** -- and on the shipped `Hexview.efi`,
117,146 bytes.

Nothing is lost by removing it. The side-by-side `.so` keeps every symbol, and
`pe-set-debug` stamps the PE debug directory to point at it, which is the chain
`rsod-decode` and any debugger follow. The `.dbgdir` section survives stripping
(it is a section, not a symbol), so that pointer is intact.

WHAT THIS GUARDS. The strip lives in the objcopy invocation of three separate
build paths -- the Makefile's link macros, `scripts/axl-cc`, and the
`axl-config.cmake` that `install.sh` generates. That is the same three-way
split `check-flag-parity` exists for, and its failure mode here is silent: an
image built by the path that forgot the flag is simply 20% larger, which
nothing notices. This gate reads the ARTIFACT rather than the command lines, so
a path that drifts is caught by its output.

Reading the artifact also catches what a flag comparison cannot: a future
objcopy that ignores the flag, or a post-processing step that reintroduces
symbols.

Usage: check-pe-stripped.py <image.efi> [...]
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

# Offsets within the PE COFF file header (after the 4-byte "PE\0\0" signature).
_COFF_PTR_TO_SYMTAB = 8    # uint32
_COFF_NUM_SYMBOLS = 12     # uint32


def coff_symbol_table(path: Path) -> tuple[int, int] | None:
    """Return (PointerToSymbolTable, NumberOfSymbols), or None if unreadable."""
    try:
        blob = path.read_bytes()
    except OSError:
        return None
    if len(blob) < 0x40 or blob[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe_off + 24 > len(blob) or blob[pe_off:pe_off + 4] != b"PE\0\0":
        return None
    coff = pe_off + 4
    ptr = struct.unpack_from("<I", blob, coff + _COFF_PTR_TO_SYMTAB)[0]
    num = struct.unpack_from("<I", blob, coff + _COFF_NUM_SYMBOLS)[0]
    return (ptr, num)


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print("check-pe-stripped: FAIL — no images given. A gate that scans")
        print("  nothing reports clean forever.")
        return 1

    findings: list[str] = []
    scanned = 0
    for name in args:
        path = Path(name)
        if not path.exists():
            findings.append(f"  {name}: not found (build it first)")
            continue
        info = coff_symbol_table(path)
        if info is None:
            findings.append(f"  {name}: not a readable PE image")
            continue
        ptr, num = info
        scanned += 1
        if ptr != 0 or num != 0:
            findings.append(
                f"  {name}: {num} COFF symbols at 0x{ptr:x} "
                f"({path.stat().st_size} bytes total)")

    if findings:
        print("check-pe-stripped: FAIL — a produced image carries a COFF "
              "symbol table")
        print("\n".join(findings))
        print()
        print("  The firmware never reads it, and it is ~20% of a small image.")
        print("  Add --strip-all to that path's objcopy. All three must have it:")
        print("    Makefile        the LINK_EFI_* macros' objcopy line")
        print("    scripts/axl-cc  the objcopy near 'output-target=$PE_TARGET'")
        print("    install.sh      the objcopy in the generated axl-config.cmake")
        print("  Symbols stay in the side-by-side .so, which is what")
        print("  pe-set-debug points the PE debug directory at.")
        return 1

    print(f"check-pe-stripped: clean — {scanned} image(s), no COFF symbol table.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
