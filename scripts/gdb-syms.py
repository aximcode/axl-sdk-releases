#!/usr/bin/env python3
"""Generate `add-symbol-file` lines from an OVMF debugcon log.

OVMF DEBUG builds print one line per loaded image to the QEMU
debugcon channel:

    Loading driver at 0x0000F0EC000 EntryPoint=0x0000F0F42CB DevicePathDxe.efi
    Loading PEIM   at 0x0000FE7C000 EntryPoint=0x0000FE91E69 DxeCore.efi

For each line, the matching `<name>.debug` ELF (from the OVMF DEBUG
build dir) is relocated by the runtime image base and printed as a
`gdb` `add-symbol-file` command. The `.text` section of every EDK2
module sits at file offset 0x240 inside the ELF, but GDB wants the
section's runtime VMA — `image_base + 0x240` for stock EDK2 modules.

Optional --axl-efi <path> appends a load line for an AXL/SDK EFI
binary loaded by the UEFI shell. Its `.text` offset is read from
the ELF (the corresponding `.so`) since AXL EFIs use the SDK linker
script which puts `.text` at a different offset (typically 0xa000).

Usage:
    scripts/gdb-syms.py <debugcon.log> \\
        --build-dir /home/mgosha/uefi/Build/OvmfX64/DEBUG_GCC5/X64 \\
        [--axl-efi out/native-x64/AxlTestNet.efi] \\
        [--axl-base 0xHEX]              # AXL load address from serial log

Output is a sequence of GDB commands suitable for `source` or
`gdb -x`.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

LOAD_RE = re.compile(
    r"Loading\s+(?:driver|PEIM)\s+at\s+0x([0-9A-Fa-f]+)\s+"
    r"EntryPoint=0x[0-9A-Fa-f]+\s+(\S+)\.efi",
    re.IGNORECASE,
)


def elf_text_vma(elf_path: Path) -> int:
    """Return the VMA of the ELF's `.text` section.

    Reads the section header table directly to avoid an objdump
    dependency. The VMA is what GDB's `add-symbol-file FILE ADDR`
    treats as the file's link-time `.text` address.
    """
    data = elf_path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"not an ELF file: {elf_path}")
    is_64 = data[4] == 2
    if not is_64:
        raise ValueError(f"only ELF64 supported: {elf_path}")
    # ELF64 header layout
    e_shoff   = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsz = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum   = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    # Section header string table
    shstr_off = e_shoff + e_shstrndx * e_shentsz
    shstr_file_off = struct.unpack_from("<Q", data, shstr_off + 0x18)[0]
    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsz
        sh_name = struct.unpack_from("<I", data, sh_off + 0x00)[0]
        sh_addr = struct.unpack_from("<Q", data, sh_off + 0x10)[0]
        # Read NUL-terminated section name
        name_start = shstr_file_off + sh_name
        name_end = data.index(b"\x00", name_start)
        if data[name_start:name_end] == b".text":
            return sh_addr
    raise ValueError(f"no .text section in {elf_path}")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("debugcon_log", type=Path,
                   help="OVMF debugcon log (from `--debugcon FILE`)")
    p.add_argument("--build-dir", type=Path, required=True,
                   help="OVMF build dir, e.g. .../DEBUG_GCC5/X64")
    p.add_argument("--axl-efi", type=Path,
                   help="AXL EFI to also load (uses sibling .so for symbols)")
    p.add_argument("--axl-base", type=lambda s: int(s, 0),
                   help="AXL EFI runtime image base (parse from serial log)")
    p.add_argument("--axl-build-dir", type=Path,
                   help="AXL build dir (e.g. out/native-x64). When set, any "
                        "`Loading driver at X NAME.efi` whose matching "
                        "NAME.so exists here is loaded automatically.")
    p.add_argument("--gdb-port", type=int, default=1234,
                   help="QEMU GDB stub port (default 1234)")
    p.add_argument("--no-target", action="store_true",
                   help="omit `target remote` line — useful when sourcing "
                        "into an already-attached gdb")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    if not args.debugcon_log.exists():
        print(f"# debugcon log not found: {args.debugcon_log}",
              file=sys.stderr)
        return 1
    if not args.build_dir.is_dir():
        print(f"# build dir not found: {args.build_dir}", file=sys.stderr)
        return 1

    out: list[str] = []
    out.append("set pagination off")
    out.append("set print pretty on")
    out.append("set confirm off")
    if not args.no_target:
        out.append(f"target remote :{args.gdb_port}")

    missing: list[str] = []
    text = args.debugcon_log.read_text(errors="replace")
    for m in LOAD_RE.finditer(text):
        base = int(m.group(1), 16)
        name = m.group(2)
        # OVMF can re-load a name across boots in the same log; we
        # re-emit add-symbol-file each time so GDB picks up the
        # last-seen base.
        debug = args.build_dir / f"{name}.debug"
        if not debug.exists():
            # Some EDK2 modules are emitted with a GUID-suffixed
            # filename (e.g. CpuDxe_<GUID>.debug) — match the prefix.
            cands = sorted(args.build_dir.glob(f"{name}_*.debug"))
            if cands:
                debug = cands[0]
            elif args.axl_build_dir is not None:
                # AXL test EFI loaded via the shell — its symbols are
                # in the matching .so in the AXL build dir, with a
                # different .text offset.
                so = args.axl_build_dir / f"{name}.so"
                if so.exists():
                    text_vma = elf_text_vma(so)
                    runtime = base + text_vma
                    out.append(f"add-symbol-file {so} 0x{runtime:x}")
                    continue
                missing.append(name)
                continue
            else:
                missing.append(name)
                continue
        try:
            text_vma = elf_text_vma(debug)
        except ValueError as exc:
            out.append(f"# skip {name}: {exc}")
            continue
        runtime = base + text_vma
        out.append(f"add-symbol-file {debug} 0x{runtime:x}")

    if args.axl_efi:
        if args.axl_base is None:
            out.append(f"# WARNING: --axl-efi without --axl-base; "
                       f"skipping {args.axl_efi.name}")
        else:
            so = args.axl_efi.with_suffix(".so")
            if not so.exists():
                out.append(f"# WARNING: missing {so} (sibling of "
                           f"{args.axl_efi.name})")
            else:
                text_vma = elf_text_vma(so)
                runtime = args.axl_base + text_vma
                out.append(f"add-symbol-file {so} 0x{runtime:x}")

    if missing:
        out.append("# missing .debug for: " + ", ".join(missing))

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
