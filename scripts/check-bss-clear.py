#!/usr/bin/env python3
"""check-bss-clear.py — assert the GCC EFI crt0 zeroes .bss itself.

axl-sdk's linker scripts emit .bss as a real NOBITS PE section; the runtime
must NOT trust the UEFI loader to zero-fill it (fresh AllocatePages memory can
be dirty — that regression stuck the mouse: 576fd474). The crt0 `_start` must
clear [_bss, _bss_end) before any C code runs.

This is a firmware-INDEPENDENT guard: it disassembles the crt0 object and
asserts `_start` references BOTH bounds (`_bss` and `_bss_end`), i.e. it loads
the span it zeroes. A boot-time probe (test-bss-probe-qemu.sh) only proves the
*loader* zeroed .bss on one firmware — it can't catch a missing crt0 clear when
the test firmware happens to hand back zeroed pages. This check can.

Usage: check-bss-clear.py <crt0.o> [<crt0.o> ...]
  objdump is chosen per object from its name (x86_64 / aarch64).
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


def objdump_for(obj: str) -> str:
    name = Path(obj).name
    if "aarch64" in name:
        return "aarch64-linux-gnu-objdump"
    if "x86_64" in name:
        return "objdump"
    return "objdump"


def start_block(disasm: str) -> str:
    """Extract the `<_start>:` function body (up to the next symbol/EOF)."""
    lines = disasm.splitlines()
    out: list[str] = []
    in_start = False
    for line in lines:
        if re.match(r"^[0-9a-f]+ <_start>:", line):
            in_start = True
            continue
        if in_start:
            # A new `<name>:` label ends the _start block.
            if re.match(r"^[0-9a-f]+ <[^>]+>:", line):
                break
            out.append(line)
    return "\n".join(out)


def check_object(obj: str) -> list[str]:
    """Return a list of problems (empty == OK)."""
    if not Path(obj).exists():
        return [f"{obj}: not found (build the crt0 first)"]
    tool = objdump_for(obj)
    try:
        disasm = subprocess.run(
            [tool, "-dr", obj],
            check=True, capture_output=True, text=True,
        ).stdout
    except FileNotFoundError:
        return [f"{obj}: {tool} not found"]
    except subprocess.CalledProcessError as exc:
        return [f"{obj}: {tool} failed: {exc.stderr.strip()}"]

    body = start_block(disasm)
    if not body:
        return [f"{obj}: no <_start> symbol in disassembly"]

    # `_bss_end` contains `_bss`, so match the upper bound first, then the
    # lower bound as `_bss` NOT followed by `_end`.
    has_end = "_bss_end" in body
    has_start = re.search(r"_bss(?!_end)\b", body) is not None
    problems: list[str] = []
    if not has_start:
        problems.append(f"{obj}: _start does not reference `_bss` "
                        "(lower bound of the .bss clear)")
    if not has_end:
        problems.append(f"{obj}: _start does not reference `_bss_end` "
                        "(upper bound of the .bss clear)")
    return problems


def main(argv: list[str]) -> int:
    objs = argv[1:]
    if not objs:
        print("usage: check-bss-clear.py <crt0.o> [<crt0.o> ...]", file=sys.stderr)
        return 2

    all_problems: list[str] = []
    for obj in objs:
        all_problems.extend(check_object(obj))

    if all_problems:
        print("check-bss-clear: FAIL — crt0 does not zero .bss "
              "(the loader is NOT trusted to zero-fill the NOBITS section):")
        for p in all_problems:
            print(f"  {p}")
        return 1

    print(f"check-bss-clear: clean — crt0 _start zeroes [_bss, _bss_end) "
          f"({len(objs)} object(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
