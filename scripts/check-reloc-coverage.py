#!/usr/bin/env python3
"""check-reloc-coverage.py — the relocation table the crt0 walks must exist, whole.

`src/crt0/axl-reloc.c` locates relocations the way a dynamic loader would: it
reads `DT_RELA` and `DT_RELASZ` out of `.dynamic` and walks `DT_RELASZ` bytes
from `DT_RELA`. It cannot notice that the bytes it is reading stopped being
relocations partway through.

That is exactly what happened on AArch64. ld synthesizes dynamic relocations
into an internal section named `.rela.dyn`; the linker script declared an
output section named `.rela`, which did NOT always absorb it. An `-frtti` link
produced TWO relocation sections at non-contiguous addresses (0x7d00 and
0xe000) while `DT_RELA` pointed at the first and `DT_RELASZ` counted BOTH. The
crt0 ran off the end of the first section and applied the following bytes as
relocations. The image faulted before `main`, with virtual calls working and
only `type_info` access broken -- about as far from the cause as a symptom
gets.

Two invariants, and the second is the one objcopy can break on its own:

  1. In the ELF, [DT_RELA, DT_RELA + DT_RELASZ) lies entirely inside ONE
     section, which starts exactly at DT_RELA. A split table fails here.
  2. That section survives into the PE image. `objcopy -j` takes exact
     section names, so renaming the output section without updating the -j
     list silently drops every relocation -- and an image with no relocations
     applied fails in ways that look like anything but a missing -j flag.

Usage: check-reloc-coverage.py <image.efi> [...]
  Each .efi is checked against its sibling .so, which is where the ELF
  dynamic info lives.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

DYN_RE = re.compile(r"^\s+(RELA|RELASZ|RELAENT)\s+(0x[0-9a-fA-F]+)")
# ` 8 .rela.dyn     00000420  0000000000011000  0000000000011000  00021000  2**3`
SEC_RE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)")


def objdump_for(path: Path) -> str:
    try:
        subprocess.run(["objdump", "-f", str(path)],
                       check=True, capture_output=True)
        return "objdump"
    except (FileNotFoundError, subprocess.CalledProcessError):
        return "aarch64-linux-gnu-objdump"


def run(tool: str, args: list[str], path: Path) -> str | None:
    try:
        return subprocess.run([tool, *args, str(path)],
                              check=True, capture_output=True, text=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


def sections(tool: str, path: Path) -> list[tuple[str, int, int]]:
    """[(name, vma, size)] for @a path."""
    out = run(tool, ["-h"], path)
    if out is None:
        return []
    found: list[tuple[str, int, int]] = []
    for line in out.splitlines():
        m = SEC_RE.match(line)
        if m is not None:
            found.append((m.group(1), int(m.group(3), 16), int(m.group(2), 16)))
    return found


def dynamic_rela(tool: str, path: Path) -> tuple[int, int] | None:
    """(DT_RELA, DT_RELASZ), or None if the image has no RELA table."""
    out = run(tool, ["-x"], path)
    if out is None:
        return None
    vals: dict[str, int] = {}
    for line in out.splitlines():
        m = DYN_RE.match(line)
        if m is not None and m.group(1) not in vals:
            vals[m.group(1)] = int(m.group(2), 16)
    if "RELA" not in vals or "RELASZ" not in vals:
        return None
    return (vals["RELA"], vals["RELASZ"])


def check(efi: Path) -> list[str]:
    so = efi.with_suffix(".so")
    if not efi.exists():
        return [f"{efi}: not found (build it first)"]
    if not so.exists():
        return [f"{efi}: no sibling {so.name}; the ELF holds the dynamic info"]

    tool = objdump_for(so)
    rela = dynamic_rela(tool, so)
    if rela is None:
        # A fully-static image with nothing to relocate is legitimate.
        return []
    addr, size = rela

    # Invariant 1: one section, starting exactly at DT_RELA, covering it all.
    holder = None
    for name, vma, sz in sections(tool, so):
        if vma == addr and sz >= size:
            holder = (name, vma, sz)
            break
    if holder is None:
        spans = [f"{n}@{v:#x}+{s:#x}" for n, v, s in sections(tool, so)
                 if v <= addr < v + s or addr <= v < addr + size]
        return [f"{so.name}: DT_RELA={addr:#x} DT_RELASZ={size:#x} is not covered "
                f"by a single section starting at DT_RELA. Overlapping: "
                f"{', '.join(spans) or 'none'}. The crt0 walks DT_RELASZ bytes "
                f"from DT_RELA and will read non-relocation bytes as "
                f"relocations."]

    # Invariant 2: objcopy carried it into the PE image.
    name = holder[0]
    for pe_name, pe_vma, pe_sz in sections(objdump_for(efi), efi):
        if pe_vma == addr and pe_sz >= size:
            return []
        if pe_name == name:
            return [f"{efi.name}: {name} is present but {pe_sz:#x} bytes at "
                    f"{pe_vma:#x}, not {size:#x} at {addr:#x}"]
    return [f"{efi.name}: section {name} (the DT_RELA table) did not survive "
            f"objcopy. `objcopy -j` takes EXACT names -- add `-j {name}`."]


def main(argv: list[str]) -> int:
    images = [Path(a) for a in argv[1:]]
    if not images:
        print("usage: check-reloc-coverage.py <image.efi> [...]", file=sys.stderr)
        return 2

    problems: list[str] = []
    for image in images:
        problems.extend(check(image))

    if problems:
        print("check-reloc-coverage: FAIL — the relocation table the crt0 walks "
              "is split or missing:")
        for p in problems:
            print(f"  {p}")
        return 1

    print(f"check-reloc-coverage: clean — DT_RELA table intact and carried in "
          f"{len(images)} image(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
