#!/usr/bin/env python3
"""make-rsod-fixture.py -- synthesize a PE + linker-map + serial-capture trio.

The MSVC/PE workflow rsod-decode.py has to handle looks nothing like the ELF
one the rest of this tree produces: a PE with no `.debug` and no PDB, a `/MAP`
linker map as the ONLY symbol source, and a crash that arrives as a whole
PuTTY capture with the dump buried in login noise. The captured data that
exposed the gaps is a vendor firmware image and cannot live in this repo, so
this builds an equivalent from scratch -- small, deterministic, and shaped to
carry every property the fixture needs to discriminate:

  * the faulting function sits ABOVE the old hard-coded 0x20000 size guess,
    so a decoder that never reads SizeOfImage calls the PC "outside all known
    images" instead of resolving it;
  * the fault lands at entry + 0xA, behind a real 3-instruction prologue, so
    "function + offset" is a different number from the RVA;
  * the branch record's TARGET is the faulting function's entry, which is what
    proves the fault happened on its first call;
  * BP is ODD, so a frame-pointer walk that only guards `fp == 0` fabricates a
    frame from it;
  * the `--wrong` variant is a plausible near-miss: same base, same symbol
    shape, DIFFERENT SizeOfImage, and one byte of padding shifted into the
    prologue so the recorded PC lands MID-INSTRUCTION. Both are independent
    proofs that the image does not belong to the dump.

Usage:
    make-rsod-fixture.py OUTDIR [--wrong] [--relocated HEX]

Writes OUTDIR/{app.efi,app.map,console.log}, plus wrong-build.{efi,map} and
console-reloc.log on request.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# ── Image geometry ────────────────────────────────────────────
#
# .text starts at RVA 0x20000 deliberately: the old code assumed a 128 KB
# image, so EVERY address in this fixture is one the unfixed decoder rejects.

IMAGE_BASE = 0x140000000
SECT_ALIGN = 0x1000
FILE_ALIGN = 0x200
TEXT_RVA = 0x20000
TEXT_SIZE = 0x2000
SIZE_OF_IMAGE = 0x30000
SIZE_OF_IMAGE_WRONG = 0x40000

# The CodeView identity the PE claims, and the path it embeds. A PE records an
# ABSOLUTE build-host path; only the basename can be looked for on the machine
# doing the decoding, which is why a renamed PDB goes unfound.
PDB_EMBEDDED_PATH = r"C:\build\obj\app.pdb"
# Deliberately NOT the GUID of any real fixture on this machine: if the
# synthetic identity collided with a real one, a reader that opened the
# wrong file would still report a match.
PDB_GUID = bytes.fromhex("11223344556677889900aabbccddeeff")
PDB_AGE = 1
# A different build's identity, for the PDB that must be REFUSED.
PDB_GUID_WRONG = bytes.fromhex("0123456789abcdef0123456789abcdef")

CALLER_RVA = 0x20000        # TestCaller
CALLEE_RVA = 0x21000        # TestFaulty
CALL_RVA = 0x2004E          # the `call TestFaulty` inside TestCaller
FAULT_RVA = 0x2100A         # TestFaulty + 0xA -- the faulting instruction

# TestFaulty's prologue, then the fault. Byte-for-byte the shape the real
# capture had: three prologue instructions totalling exactly 0xA bytes, then a
# 9-byte %gs-relative load. The 0xA matters -- it is what "+ 0xa" must report.
PROLOGUE = bytes([
    0x48, 0x89, 0x5C, 0x24, 0x08,     # mov %rbx,0x8(%rsp)
    0x57,                             # push %rdi
    0x48, 0x83, 0xEC, 0x30,           # sub $0x30,%rsp
])
FAULT_INSN = bytes([
    0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00,   # mov %gs:0x58,%rax
])
# Two 10-byte movabs immediately after the fault, deliberately. A disassembler
# that stops a fixed 24 bytes past the faulting PC lands INSIDE the second one
# and renders the tail as `rex.W` / `.byte 0x89` -- which reads like it lost
# its place at the crash site. Short trailing instructions would hide that.
TRAILING = bytes([
    0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  # movabs ...,%rax
    0x48, 0xB9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  # movabs ...,%rcx
])
EPILOGUE = bytes([
    0x48, 0x83, 0xC4, 0x30,           # add $0x30,%rsp
    0x5F,                             # pop %rdi
    0xC3,                             # ret
])


def build_text(wrong: bool) -> bytes:
    """The .text section image, laid out at TEXT_RVA."""
    text = bytearray(b"\x90" * TEXT_SIZE)

    # TestCaller: nop sled up to the call site, then `call TestFaulty`.
    call_off = CALL_RVA - TEXT_RVA
    rel = CALLEE_RVA - (CALL_RVA + 5)
    text[call_off:call_off + 5] = b"\xE8" + struct.pack("<i", rel)
    text[call_off + 5] = 0xC3          # ret, so the caller is a real function

    # TestFaulty. In the WRONG build one extra byte of prologue shifts
    # everything after it by one, so the dump's recorded PC (fixed at
    # FAULT_RVA) no longer lands on an instruction boundary here -- exactly how
    # a different build configuration of the same source misleads the decoder.
    body = PROLOGUE + FAULT_INSN + TRAILING + EPILOGUE
    if wrong:
        body = b"\x90" + body
    off = CALLEE_RVA - TEXT_RVA
    text[off:off + len(body)] = body
    return bytes(text)


def build_pe(wrong: bool) -> bytes:
    """A minimal but genuinely well-formed PE32+ that objdump can disassemble."""
    text = build_text(wrong)
    size_of_image = SIZE_OF_IMAGE_WRONG if wrong else SIZE_OF_IMAGE

    # Headers: DOS stub (PE header at 0x80) + PE sig + COFF + optional + 1 section.
    pe_off = 0x80
    opt_size = 240                                  # PE32+ with 16 data dirs
    headers_len = pe_off + 4 + 20 + opt_size + 2 * 40
    size_of_headers = (headers_len + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)
    raw_size = (len(text) + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    dos = bytearray(pe_off)
    dos[0:2] = b"MZ"
    dos[0x3C:0x40] = struct.pack("<I", pe_off)

    coff = struct.pack(
        "<HHIIIHH",
        0x8664,          # Machine = AMD64
        2,               # NumberOfSections (.text, .rdata)
        0x6A8F3DB9,      # TimeDateStamp (fixed -- the fixture is deterministic)
        0, 0,            # symbol table (none)
        opt_size,
        0x022E,          # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | ...
    )

    opt = struct.pack(
        "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
        0x20B,           # PE32+
        14, 0,           # linker version
        raw_size,        # SizeOfCode
        0, 0,            # initialized / uninitialized data
        CALLER_RVA,      # AddressOfEntryPoint
        TEXT_RVA,        # BaseOfCode
        IMAGE_BASE,
        SECT_ALIGN, FILE_ALIGN,
        0, 0, 0, 0, 0, 0,      # OS / image / subsystem versions
        0,               # Win32VersionValue
        size_of_image,
        size_of_headers,
        0,               # CheckSum
        10,              # Subsystem = EFI application
        0x0160,          # DllCharacteristics (NX_COMPAT et al)
        0x100000, 0x1000, 0x100000, 0x1000,
        0,               # LoaderFlags
        16,              # NumberOfRvaAndSizes
    )
    # Data directory 6 is IMAGE_DIRECTORY_ENTRY_DEBUG. Filled in below once the
    # debug directory's offset is known; the rest stay empty.
    opt += b"\x00" * 128
    assert len(opt) == opt_size, len(opt)

    # .rdata carries the CodeView record and the debug directory that points at
    # it -- where MSVC puts them. Its raw data follows .text's in the file.
    rdata_rva = TEXT_RVA + TEXT_SIZE
    rdata_raw_off = size_of_headers + raw_size
    cv = (b"RSDS" + PDB_GUID + struct.pack("<I", PDB_AGE)
          + PDB_EMBEDDED_PATH.encode() + b"\x00")
    debug_dir = struct.pack(
        "<IIHHIII",
        0, 0x6A8F3DB9,   # Characteristics, TimeDateStamp
        0, 0,            # Major/MinorVersion
        2,               # IMAGE_DEBUG_TYPE_CODEVIEW
        len(cv),
        rdata_rva,       # AddressOfRawData: the CV record starts .rdata
    ) + struct.pack("<I", rdata_raw_off)              # PointerToRawData
    assert len(debug_dir) == 28, len(debug_dir)
    rdata = cv + debug_dir
    rdata_size = (len(rdata) + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)
    debug_rva = rdata_rva + len(cv)                   # where debug_dir lands

    section = struct.pack(
        "<8sIIIIIIHHI",
        b".text",
        TEXT_SIZE,       # VirtualSize
        TEXT_RVA,
        raw_size,
        size_of_headers,  # PointerToRawData
        0, 0, 0, 0,
        0x60000020,      # CODE | EXECUTE | READ
    ) + struct.pack(
        "<8sIIIIIIHHI",
        b".rdata",
        len(rdata),
        rdata_rva,
        rdata_size,
        rdata_raw_off,
        0, 0, 0, 0,
        0x40000040,      # INITIALIZED_DATA | READ
    )

    out = bytearray()
    out += dos
    out += b"PE\x00\x00" + coff + opt + section
    out += b"\x00" * (size_of_headers - len(out))
    # IMAGE_DIRECTORY_ENTRY_DEBUG -> (rva, size)
    dd = pe_off + 4 + 20 + 112 + 6 * 8
    out[dd:dd + 8] = struct.pack("<II", debug_rva, len(debug_dir))
    out += text
    out += b"\x00" * (raw_size - len(text))
    out += rdata
    out += b"\x00" * (rdata_size - len(rdata))
    return bytes(out)


def build_pdb(guid: bytes = PDB_GUID, age: int = PDB_AGE) -> bytes:
    """A minimal but structurally valid MSF 7.00 container.

    Six 4096-byte blocks: the superblock, the two free-block maps MSF always
    reserves at 1 and 2, the PDB Info stream, the stream directory, and the
    directory's block map. Only stream 1 carries anything -- Version,
    Signature, Age, GUID -- because the identity is the whole point.
    """
    bs = 4096
    info = struct.pack("<III", 20000404, 0, age) + guid   # VC70 Info stream

    # Stream directory: count, per-stream sizes, then each stream's blocks.
    # Stream 0 is the old directory and is conventionally empty.
    directory = struct.pack("<I", 2)                      # NumStreams
    directory += struct.pack("<II", 0, len(info))         # sizes
    directory += struct.pack("<I", 3)                     # stream 1 -> block 3

    blocks = [bytearray(b"\x00" * bs) for _ in range(6)]

    magic = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
    assert len(magic) == 32, len(magic)
    superblock = magic + struct.pack(
        "<IIIIII",
        bs,          # BlockSize
        1,           # FreeBlockMapBlock
        6,           # NumBlocks
        len(directory),
        0,           # Unknown
        5,           # BlockMapAddr
    )
    blocks[0][:len(superblock)] = superblock
    blocks[3][:len(info)] = info
    blocks[4][:len(directory)] = directory
    blocks[5][:4] = struct.pack("<I", 4)                  # directory is block 4
    return b"".join(bytes(b) for b in blocks)


def build_map(wrong: bool) -> str:
    """An MSVC `/MAP` linker map -- no `/MAPINFO:LINES`, so no line numbers.

    The symbol names differ between builds on purpose: handed the wrong image
    the decoder answers `WrongFunc`, confidently and wrongly, which is the
    failure the size / instruction-boundary gates exist to catch.
    """
    callee = "?WrongFunc@@YAXXZ" if wrong else "?TestFaulty@@YAPEAXPEAI@Z"
    caller = "?WrongCaller@@YAHH@Z" if wrong else "?TestCaller@@YAHH@Z"
    size = SIZE_OF_IMAGE_WRONG if wrong else SIZE_OF_IMAGE
    return f"""\
 test

 Timestamp is 6a8f3db9 (Wed Aug 26 14:25:45 2026)

 Preferred load address is {IMAGE_BASE:016x}

 Start         Length     Name                   Class
 0001:00000000 {TEXT_SIZE:08x}H .text                   CODE
 0002:00000000 {size:08x}H .data                   DATA

  Address         Publics by Value              Rva+Base               Lib:Object

 0001:00000000       {caller:<26} {IMAGE_BASE + CALLER_RVA:016x} f   TestLib.obj
 0001:00001000       {callee:<26} {IMAGE_BASE + CALLEE_RVA:016x} f   TestLib.obj
 0002:00000000       ?gTestGlobal@@3HA          {IMAGE_BASE + 0x22000:016x}     TestLib.obj

 entry point at        0001:00000000
"""


def build_console_log(load_base: int = IMAGE_BASE) -> str:
    """A whole PuTTY capture with the dump buried in it.

    Everything outside the dump is noise the decoder has to see past, and the
    dump itself carries the four things the report needs: the branch records,
    an ODD BP, a stack window holding real return addresses, and the firmware's
    own loaded-image list (the only authority on where the image was loaded and
    how big it was).
    """
    fault_pc = load_base + FAULT_RVA
    lbr_from = load_base + CALL_RVA
    lbr_to = load_base + CALLEE_RVA
    ret_addr = load_base + CALL_RVA + 5       # return address of that call

    # A stack window that REACHES the odd BP.
    #
    # This is load-bearing and was wrong once. The frame-pointer walk rejects a
    # pointer outside the captured stack before it ever considers alignment, so
    # a short window makes the odd-BP guard untestable: sabotaging the guard
    # away left the suite green, because the range check was quietly doing the
    # work. The dump must therefore span far enough that BP = 0x452AF2D9 lands
    # INSIDE it, and carry a plausible saved-FP / return-address pair at the
    # misaligned read that BP implies -- which is how the original capture
    # produced its fabricated `#1 0x1579724` frame.
    stack_lo, stack_hi = 0x452AF1B0, 0x452AF300
    values: dict[int, int] = {
        0x452AF1B8: ret_addr,                       # a real return address
        0x452AF1C8: 0x0000000000000004,
        0x452AF230: 0x00000000452AF2D9,             # BP, spilled on the stack
        0x452AF238: load_base + CALLER_RVA,         # the caller's entry
        0x452AF2D8: 0x00000000452D3C99,
        0x452AF2E0: 0x0000000157972400,             # BP+8 reads INTO this
    }
    stack = [f"  {a:08X}  {values.get(a, 0):016X} ........"
             for a in range(stack_lo, stack_hi, 8)]

    stack_text = "\n".join(stack)
    return f"""\
=~=~=~=~=~=~=~=~=~=~=~= PuTTY log 2026.08.27 08:01:47 =~=~=~=~=~=~=~=~=~=~=~=
login as: root
Keyboard-interactive authentication prompts from server:
| Password:
End of keyboard-interactive prompts from server

racadm>>console com2
Connected to Serial Device 2. To end type: ^\\

08/27/2026 13:00:26
TestSystem - BIOS 0.1.18
A system restart is required. The system detected an exception during the UEFI
pre-boot environment.
-------------------------------------------------------------------------------
Type: Page fault (14) Source: Software (UEFI0012) on BSP
AX=0000000000000000 BX=0000000000000000 SI=0000000000000000 DI=0000000000000000
CX=0000000140022000 DX=0000000000000000 R8=0000000000000000 R9=0000000000000000
10=0000000000000002 11=00000000452AF300 12=0000000000040300 13=0000000000000000
14=0000000000000000 15=0000000000000000 BP=00000000452AF2D9 SP=00000000452AF1B0
IP={fault_pc:016X} Flags=00010202  CurrentTPL = 04, LastEventTime 00000021E79C
LastMsg:

LBRfr0 {lbr_from:X} test.efi +{CALL_RVA:06X}
LBRto0 {lbr_to:X} test.efi +{CALLEE_RVA:06X}
-->RIP {fault_pc:X} test.efi +{FAULT_RVA:06X}
       Stack trace not available

Stack Dump:
{stack_text}

Loaded images:
  40C15000 00007B80 SomeOtherDxe.efi
  {load_base:09X} {SIZE_OF_IMAGE:08X} test.efi

Log Size: 4242
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("outdir", help="directory to write the fixture into")
    ap.add_argument("--wrong", action="store_true",
                    help="also emit the near-miss wrong-build.{efi,map} pair")
    ap.add_argument("--pdb", action="store_true",
                    help="also emit app.pdb (identity matching the PE) and "
                         "mismatched.pdb (a different build's identity)")
    ap.add_argument("--relocated", metavar="HEX",
                    help="also emit console-reloc.log, a capture of the SAME "
                         "image loaded at a base other than its preferred one")
    args = ap.parse_args()

    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    (out / "app.efi").write_bytes(build_pe(wrong=False))
    (out / "app.map").write_text(build_map(wrong=False))
    (out / "console.log").write_text(build_console_log())
    if args.wrong:
        (out / "wrong-build.efi").write_bytes(build_pe(wrong=True))
        (out / "wrong-build.map").write_text(build_map(wrong=True))
    if args.pdb:
        (out / "app.pdb").write_bytes(build_pdb())
        (out / "mismatched.pdb").write_bytes(build_pdb(guid=PDB_GUID_WRONG))
    if args.relocated:
        (out / "console-reloc.log").write_text(
            build_console_log(int(args.relocated, 16)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
