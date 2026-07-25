#!/usr/bin/env python3
from __future__ import annotations

"""
UEFI RSOD (Red Screen of Death) Decoder
========================================

Parses UEFI exception handler output from EDK2 firmware and resolves
crash addresses to source file/line/function using debug symbols.

This is the SDK's zero-dependency lightweight decoder — pure stdlib Python
that shells out to the system `gdb` / binutils, so it packages cleanly into
the axl-sdk host-tools bundle with no pip install. For the full-featured tool
(pyelftools/capstone symbolization, PE+PDB minidump analysis for MSVC crashes,
LLDB integration, callsite-arg / tail-call reconstruction, and a web UI), use
the standalone project: https://github.com/aximcode/rsod-decode — grab the
self-contained `rsod.pyzw` from its Releases. The two are separate tools with
different dependency footprints, not two copies of one thing.

OVERVIEW
--------
When a UEFI application crashes, EDK2 firmware prints an exception dump
(the "Red Screen of Death") to the serial console with register values,
a fault address, and on AARCH64 a frame-pointer-walked stack trace.

This script parses that text, matches addresses to debug images (.debug,
.dll, .so, or .efi files), and produces a human-readable crash report
with function names, source locations, disassembly, and a diagnosis of
the probable cause.

SUPPORTED RSOD FORMATS
----------------------
X64 (UefiCpuPkg CpuExceptionHandlerLib):
    !!!! X64 Exception Type - 0e(#PF)  CPU Apic ID - 00000000 !!!!
    RIP  - 000000006A3C02EB, CS  - ...
    RAX  - ..., RCX - ..., RDX - ...
    ...
    !!!! Find image based on IP(0x...) ... (ImageBase=...) !!!!

AARCH64 (ArmPkg DefaultExceptionHandlerLib):
    Synchronous Exception at 0x...
    PC 0x... (0xBASE+0xOFFSET) [ N] Module.dll
    PC 0x... (0xBASE+0xOFFSET) [ N] Module.dll
    X0 0x...  X1 0x...  ...
    SP 0x...  ELR 0x...  ESR 0x...  FAR 0x...
    ESR : EC 0x25  IL 0x1  ISS 0x...
    Data abort: Translation fault, third level

ARCHITECTURE
------------
The script is organized in layers, each with a single responsibility:

    CLI / Args          Parse command-line arguments
         |
    RSOD Parsers        Parse text -> structured RsodData
    (parse_x64_rsod,    Extract exception type, registers, stack PCs,
     parse_aarch64)     ESR decode, stack dump qwords, module bases
         |
    Image Registry      Manage multiple debug images
    (register_image,    Resolve .efi -> .dll -> .debug chain
     resolve_addr_to    Match addresses to the correct image
     _image)
         |
    Symbol Resolution   Address -> function/file/line
    (resolve_address,   GDB backend (preferred): gdb.block_for_pc()
     GdbBackend,         for inline-aware resolution + info line
     _resolve_address    for address-specific file:line
     _binutils)         Binutils fallback: addr2line -> nm -> nm -D
         |
    Exception Diagnosis Interpret exception + fault address
    (_diagnose_x64,     X64: vector -> name, CR2 -> NULL/near-NULL
     _diagnose_aarch64) AARCH64: ESR EC -> name, FAR -> NULL/near-NULL
         |
    Output Formatters   Render results for the user
    (emit_human,        Human: colored terminal output with OSC 8 links
     emit_markdown,     Markdown: tables with GitHub/relative links
     emit_json)         JSON: machine-readable structured output

SYMBOL RESOLUTION
-----------------
The script uses gdb as its primary backend (if available on PATH),
falling back to addr2line/nm/objdump if gdb is not installed.

GDB backend (preferred):
    - Single gdb -batch invocation per image resolves all addresses
    - Uses gdb.block_for_pc() Python API for inline-aware function names
    - Uses "info line *OFFSET" for address-specific file:line
    - Uses "info symbol OFFSET" for stripped binaries (dynamic symbols)
    - Uses "disas /s" for interleaved source + disassembly

Binutils fallback:
    - addr2line -C -f -i -p for DWARF resolution with inlines
    - nm -C -n for static symbol table lookup
    - nm -D -C for dynamic symbol table (stripped binaries)
    - objdump -d for disassembly

Symbol results are cached per-ELF to avoid redundant subprocess calls.

IMAGE FILE TYPES
----------------
The --image flag accepts any of these file types:

    .debug  ELF with full DWARF debug info (best)
    .dll    ELF with gnu_debuglink to .debug (EDK2 intermediate)
    .so     ELF shared library (same as .dll)
    .efi    PE/COFF binary — script extracts the embedded PDB/DLL path
            and follows gnu_debuglink to find the .debug ELF

Architecture is auto-detected from ELF/PE magic bytes (no external
`file` command needed — works on Windows).

OUTPUT MODES
------------
Compact (default):
    Cause, fault address, crash location one-liner, stack trace with
    function/file:line, register code-pointer annotations.

Detailed (--detail):
    Everything in compact mode, plus: raw exception data, ESR decode,
    disassembly with interleaved source around fault instruction,
    stack memory scan for return addresses, image paths and bases.

Markdown (--markdown):
    Standalone .md document with tables, GitHub links (auto-detected
    from git or via --repo/--commit), and fenced code blocks for
    disassembly. Suitable for pasting into issues or reports.

JSON (--json):
    Machine-readable output with all resolved data. No external
    dependencies beyond gdb/binutils.

SOURCE PATH REMAPPING
---------------------
Debug ELFs contain absolute paths from the build machine. When the
source is at a different location locally, use --source-root:

    rsod-decode.py --source-root ~/projects/edk2 --image app.debug ...

The script tries progressively shorter suffixes of the DWARF path
against the source root until it finds a matching file. Results are
cached for performance.

CLICKABLE LINKS
---------------
In terminal output, file:line locations are wrapped in OSC 8 hyperlinks
(when stdout is a terminal). These are clickable in:
    - VS Code integrated terminal
    - iTerm2
    - Windows Terminal
    - GNOME Terminal 3.26+

In markdown output, links target GitHub (auto-detected from git remote)
or use relative paths that work in VS Code markdown preview.

EXCEPTION DIAGNOSIS
-------------------
The script interprets the raw exception type and fault address into a
human-readable cause. Examples:

    X64 #PF + CR2=0x0        -> "Page fault — NULL pointer dereference"
    X64 #PF + CR2=0x18       -> "Page fault — Near-NULL dereference
                                  (offset 0x18 — likely struct member
                                  access via NULL pointer)"
    X64 #UD                  -> "Invalid opcode — possible
                                  __builtin_trap() or stack corruption"
    X64 #GP                  -> "General protection fault"
    AARCH64 EC=0x25 + FAR=0  -> "Data abort from same EL — NULL pointer
                                  dereference"
    AARCH64 EC=0x21          -> "Instruction abort — jumped to
                                  unmapped/non-executable address"

MULTI-IMAGE SUPPORT
-------------------
Real crashes often span multiple UEFI modules. Pass --image multiple
times, optionally with per-image base addresses:

    rsod-decode.py --image App.debug \\
                   --image Shell.debug:0x7E212000 \\
                   --image DxeCore.debug:0x47683000 \\
                   --file rsod.txt

AARCH64 RSODs include per-frame module names and bases, which the
script uses to match addresses to the correct image automatically.
Unresolved frames show "module+offset (no debug image)".

EXAMPLES
--------
Parse an RSOD from a serial log file:
    rsod-decode.py --image IpmiTool.efi --file rsod_log.txt

Pipe RSOD text from clipboard:
    pbpaste | rsod-decode.py --image IpmiTool.debug

Detailed output with disassembly:
    rsod-decode.py --image app.debug --detail --file rsod.txt

Markdown report with GitHub links:
    rsod-decode.py --image app.debug --markdown --file rsod.txt > crash.md

Manual address decode:
    rsod-decode.py --image app.debug --base 0x6A3C0000 --addr 0x6A3C02EB

JSON output:
    rsod-decode.py --image app.debug --json --file rsod.txt

Remap build paths to local source:
    rsod-decode.py --image app.debug --source-root ~/projects/edk2 --file rsod.txt

REQUIREMENTS
------------
Python 3.7+ (no pip dependencies).

For symbol resolution, one of:
    - gdb (preferred — single tool, inline-aware, handles stripped binaries)
    - binutils (addr2line + nm + objdump — fallback)

On Linux:   gdb is usually pre-installed; binutils via package manager
On Windows: install via MSYS2 (pacman -S mingw-w64-x86_64-binutils)
On macOS:   install via Homebrew (brew install gdb binutils)

For AARCH64 cross-debugging on x86:
    Linux: sudo dnf install gcc-aarch64-linux-gnu (or apt equivalent)
    Regular gdb handles AARCH64 ELFs for symbol resolution; only
    disassembly needs "set architecture aarch64" (handled automatically).
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ═══════════════════════════════════════════════════════════════
# Data model
# ═══════════════════════════════════════════════════════════════

@dataclass
class ResolveResult:
    func: str = "???"
    file: str = ""
    line: int = 0
    status: str = "unknown"  # resolved, symbol_only, dynamic_symbol, unknown
    inlines: list[tuple[str, str]] = field(default_factory=lambda: list[tuple[str, str]]())


@dataclass
class Image:
    elf: str = ""
    base: Optional[int] = None
    name: str = ""
    size: int = 0x20000
    map_file: str = ""     # linker .map file (function-level resolution)
    efi_path: str = ""     # original .efi path (for PE DWARF resolution)
    pe_base: int = 0       # preferred load address from .map or PE header


@dataclass
class StackFrame:
    frame: int = 0
    addr: int = 0
    offset: int = 0
    image_idx: int = -1
    module_name: str = ""  # from RSOD text, used when no --image matches
    resolve: ResolveResult = field(default_factory=ResolveResult)


@dataclass
class RegAnnotation:
    reg: str = ""
    addr: int = 0
    resolve: ResolveResult = field(default_factory=ResolveResult)


@dataclass
class RsodData:
    arch: str = ""
    exception_type: str = ""
    exception_data: str = ""
    fault_pc: str = ""
    fault_addr: str = ""
    esr_decode: str = ""
    abort_desc: str = ""
    cause: str = ""  # human-readable diagnosis
    parsed_base: Optional[int] = None
    stack_pcs: list[int] = field(default_factory=lambda: list[int]())
    # Per-frame info from AARCH64 RSOD: [(pc, module_name, base, offset), ...]
    stack_frame_info: list[tuple[int, str, int, int]] = field(
        default_factory=lambda: list[tuple[int, str, int, int]]())
    registers: dict[str, int] = field(default_factory=lambda: dict[str, int]())
    stack_qwords: list[int] = field(default_factory=lambda: list[int]())
    # (stack_address, value) pairs — the ADDRESSED stack dump, needed to walk a
    # frame-pointer chain (following a saved-FP link means reading the value AT
    # the address the FP points to). stack_qwords is values-only; this keeps the
    # addresses so recover_backtrace_via_fp can reconstruct an ordered trace
    # when the firmware printed no sNN frames.
    stack_pairs: list[tuple[int, int]] = field(
        default_factory=lambda: list[tuple[int, int]]())
    module_bases: dict[str, int] = field(default_factory=lambda: dict[str, int]())
    # True when the backtrace was reconstructed by walking the FP chain (no
    # firmware sNN frames) — the order is heuristic, so the report flags it.
    recovered_via_fp: bool = False


# ═══════════════════════════════════════════════════════════════
# Toolchain
# ═══════════════════════════════════════════════════════════════

class Toolchain:
    def __init__(self, arch: str):
        self.arch = arch
        if arch == "AARCH64":
            self.prefix = "aarch64-linux-gnu-"
        else:
            self.prefix = ""
        self.addr2line = f"{self.prefix}addr2line"
        self.nm = f"{self.prefix}nm"
        self.objdump = f"{self.prefix}objdump"
        # Prefer gdb for symbol resolution (single tool, handles stripped binaries)
        self.gdb = "gdb" if _which("gdb") else ""
        self.use_gdb = bool(self.gdb)

    def check(self):
        if self.use_gdb:
            return  # gdb is sufficient for all resolution
        for tool in [self.addr2line, self.nm]:
            if not _which(tool):
                print(f"Error: {tool} not found on PATH (install gdb or binutils)", file=sys.stderr)
                is_win = sys.platform == "win32"
                if self.arch == "AARCH64":
                    if is_win:
                        print("Install: MSYS2 with 'pacman -S mingw-w64-x86_64-binutils aarch64-none-elf-binutils'", file=sys.stderr)
                    else:
                        print("Install: sudo dnf install gcc-aarch64-linux-gnu  (or apt install gcc-aarch64-linux-gnu)", file=sys.stderr)
                else:
                    if is_win:
                        print("Install: MSYS2 with 'pacman -S mingw-w64-x86_64-binutils'", file=sys.stderr)
                        print("  or add your GCC toolchain's bin/ directory to PATH", file=sys.stderr)
                    else:
                        print("Install: sudo dnf install binutils  (or apt install binutils)", file=sys.stderr)
                sys.exit(1)


def _which(cmd: str) -> bool:
    """Check if a command exists on PATH (cross-platform)."""
    return shutil.which(cmd) is not None


def _run(cmd: list[str]) -> str:
    """Run a command and return stdout, or empty string on failure."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return r.stdout if r.returncode == 0 else ""
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ""


# ═══════════════════════════════════════════════════════════════
# GDB batch backend — replaces addr2line/nm/objdump with one tool
# ═══════════════════════════════════════════════════════════════

_RESOLVE_MARKER = ">>>RESOLVE "

class GdbBackend:
    """Batch gdb interface for symbol resolution, disassembly, and source display."""

    def __init__(self, gdb_cmd: str, elf: str, arch: str):
        self.gdb = gdb_cmd
        self.elf = elf
        self.arch = arch
        self._cache: Dict[str, ResolveResult] = {}

    def _gdb_run(self, commands: List[str]) -> str:
        """Run gdb -batch with a list of -ex commands."""
        args = [self.gdb, "-batch"]
        if self.arch == "AARCH64":
            args += ["-ex", "set architecture aarch64"]
        for cmd in commands:
            args += ["-ex", cmd]
        args.append(self.elf)
        return _run(args)

    def resolve_batch(self, offsets: List[str]) -> None:
        """Resolve multiple offsets in a single gdb invocation with inline awareness."""
        if not offsets:
            return
        # Build a Python script and pipe it to gdb via stdin.
        # Uses 'python exec(open(0).read())' to read from stdin portably.
        marker = _RESOLVE_MARKER
        # Combine two gdb sources:
        # 1. "info line *OFFSET" for the address-specific file:line
        # 2. block_for_pc() for the innermost function name + inline chain
        script = f"""import gdb, re
offsets = {offsets!r}
marker = "{marker}"
for off in offsets:
    addr = int(off, 16)
    print(marker + off)
    try:
        # Get address-specific file:line from info line
        il = gdb.execute("info line *" + off, to_string=True).strip()
        m = re.match(r'Line (\\d+) of "([^"]+)"', il)
        if m:
            src_line = int(m.group(1))
            src_file = m.group(2)
        else:
            src_line = 0
            src_file = ""

        # Get innermost function name + inline chain from block_for_pc
        blk = gdb.block_for_pc(addr)
        found = False
        while blk:
            if blk.function:
                fn = blk.function
                st = fn.symtab
                ffile = st.filename if st else ""
                if not found:
                    # Use block_for_pc for function name, info line for file:line
                    print("FUNC:" + fn.name)
                    print("FILE:" + (src_file if src_file else ffile))
                    print("LINE:" + str(src_line if src_line else fn.line))
                    found = True
                else:
                    print("INLINE:" + fn.name + "|" + ffile + ":" + str(fn.line))
            blk = blk.superblock
        if not found:
            if src_file:
                # info line worked but no block — use what we have
                sym = gdb.execute("info symbol " + str(addr), to_string=True).strip()
                sm = re.match(r"(\\S+)", sym)
                fname = sm.group(1) if sm else "???"
                print("FUNC:" + fname)
                print("FILE:" + src_file)
                print("LINE:" + str(src_line))
            else:
                print("NOLINE:")
                sym = gdb.execute("info symbol " + str(addr), to_string=True).strip()
                print("SYM:" + sym)
    except Exception as e:
        print("ERR:" + str(e))
"""
        args = [self.gdb, "-batch", "-ex", "python exec(open(0).read())", self.elf]
        try:
            r = subprocess.run(args, input=script, capture_output=True, text=True, timeout=30)
            out = r.stdout if r.returncode == 0 else ""
        except (subprocess.TimeoutExpired, FileNotFoundError):
            out = ""
        self._parse_batch_output(out)

    def _parse_batch_output(self, output: str) -> None:
        """Parse marker-separated output from gdb Python resolve script."""
        sections = output.split(_RESOLVE_MARKER)
        for section in sections[1:]:
            lines = section.strip().splitlines()
            if not lines:
                continue
            offset_hex = lines[0].strip()
            result = ResolveResult()

            for line in lines[1:]:
                if line.startswith("FUNC:"):
                    result.func = line[5:]
                    result.status = "resolved"
                elif line.startswith("FILE:"):
                    result.file = line[5:]
                elif line.startswith("LINE:"):
                    try:
                        result.line = int(line[5:])
                    except ValueError:
                        pass
                elif line.startswith("INLINE:"):
                    parts = line[7:].split("|", 1)
                    ifunc = parts[0]
                    ifline = parts[1] if len(parts) > 1 else ""
                    result.inlines.append((ifunc, ifline))
                elif line.startswith("SYM:"):
                    sym_text = line[4:]
                    m = re.match(r"(\S+) (?:\+ \d+ )?in section", sym_text)
                    if m and result.status == "unknown":
                        result.func = m.group(1)
                        result.status = "symbol_only"

            self._cache[offset_hex] = result

    def resolve(self, offset_hex: str) -> ResolveResult:
        """Resolve a single offset. Uses cache, or does a one-off gdb call."""
        if offset_hex.startswith("0x-") or offset_hex.startswith("-"):
            return ResolveResult()
        if offset_hex in self._cache:
            return self._cache[offset_hex]
        # Single resolve (shouldn't happen if resolve_batch was called first)
        self.resolve_batch([offset_hex])
        return self._cache.get(offset_hex, ResolveResult())

    def disassemble(self, offset: int) -> List[str]:
        """Return disassembly lines around offset. Uses gdb disas /s for interleaved source."""
        # Find function start
        start = offset
        out = self._gdb_run([f"info symbol {offset}"])
        m = re.search(r"(\S+) \+ (\d+) in section", out)
        if m:
            start = offset - int(m.group(2))
        elif re.search(r"(\S+) in section", out):
            start = offset  # at function start

        stop = offset + 24
        out = self._gdb_run([f"disas /s {start},{stop}"])
        if not out or "Dump of assembler" not in out:
            return []

        fault_addr = offset
        result: List[str] = []
        for line in out.splitlines():
            # Assembly lines: "   0x00000000000002eb <Func+0>:  instruction"
            m_asm = re.match(r"\s+(0x[0-9a-f]+)\s+<", line)
            if m_asm:
                addr_val = int(m_asm.group(1), 16)
                display = re.sub(r"^\s+0x[0-9a-f]+\s+", "  ", line)
                if addr_val == fault_addr:
                    result.append(f"  {C_RED}>>>{C_NC} {display}")
                else:
                    result.append(f"      {display}")
            # Source lines (from disas /s): "NN\tsource code"
            elif re.match(r"\d+\t", line):
                result.append(f"  {C_DIM}{line}{C_NC}")

        return result

    def source_context(self, offset: int) -> List[str]:
        """Return source lines around the fault address."""
        out = self._gdb_run([f"list *{offset}"])
        if not out:
            return []

        result: List[str] = []
        first_line = out.splitlines()[0] if out.splitlines() else ""

        # Determine the fault line number from "0xADDR is in FUNC (FILE:LINE)."
        fault_line = 0
        m2 = re.search(r":(\d+)\)", first_line)
        if m2:
            fault_line = int(m2.group(1))

        for line in out.splitlines()[1:]:
            m = re.match(r"(\d+)\t(.*)", line)
            if m:
                n = int(m.group(1))
                content = m.group(2)
                if n == fault_line:
                    result.append(f"  {C_RED}>>>{n:4d}{C_NC} {content}")
                else:
                    result.append(f"  {C_DIM}   {n:4d}{C_NC} {content}")

        return result


# ═══════════════════════════════════════════════════════════════
# Image registry
# ═══════════════════════════════════════════════════════════════

def _detect_binary_type(path: str) -> str:
    """Detect file type from magic bytes. Returns 'elf', 'pe', or ''."""
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic[:4] == b"\x7fELF":
                return "elf"
            if magic[:2] == b"MZ":
                return "pe"
    except OSError:
        pass
    return ""


def _detect_elf_machine(path: str) -> str:
    """Read ELF e_machine field to determine architecture."""
    try:
        with open(path, "rb") as f:
            ident = f.read(20)
            if len(ident) < 20 or ident[:4] != b"\x7fELF":
                return ""
            ei_data = ident[5]   # 1=little-endian, 2=big-endian
            fmt = "<H" if ei_data == 1 else ">H"
            # e_machine is at offset 18 in both ELF32 and ELF64
            machine = struct.unpack(fmt, ident[18:20])[0]
            if machine == 0xB7:   # EM_AARCH64
                return "AARCH64"
            if machine == 0x3E:   # EM_X86_64
                return "X64"
            if machine == 0x03:   # EM_386
                return "IA32"
    except OSError:
        pass
    return ""


def _detect_pe_machine(path: str) -> str:
    """Read PE Machine field to determine architecture."""
    try:
        with open(path, "rb") as f:
            mz = f.read(64)
            if len(mz) < 64 or mz[:2] != b"MZ":
                return ""
            # PE header offset is at MZ+0x3C (little-endian DWORD)
            pe_offset = struct.unpack("<I", mz[0x3C:0x40])[0]
            f.seek(pe_offset)
            pe_sig = f.read(4)
            if pe_sig != b"PE\x00\x00":
                return ""
            machine = struct.unpack("<H", f.read(2))[0]
            if machine == 0xAA64:  # IMAGE_FILE_MACHINE_ARM64
                return "AARCH64"
            if machine == 0x8664:  # IMAGE_FILE_MACHINE_AMD64
                return "X64"
            if machine == 0x014C:  # IMAGE_FILE_MACHINE_I386
                return "IA32"
    except OSError:
        pass
    return ""


def _extract_pe_strings(path: str, min_len: int = 8) -> list[str]:
    """Extract printable ASCII strings from a binary file (replaces `strings`)."""
    result: list[str] = []
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return result

    current: list[str] = []
    for byte in data:
        if 0x20 <= byte < 0x7F:
            current.append(chr(byte))
        else:
            if len(current) >= min_len:
                result.append("".join(current))
            current = []
    if len(current) >= min_len:
        result.append("".join(current))
    return result


def detect_file_arch(path: str) -> str:
    """Detect architecture from ELF/PE magic bytes (no external tools)."""
    real_path = os.path.realpath(path)
    btype = _detect_binary_type(real_path)
    if btype == "elf":
        return _detect_elf_machine(real_path)
    if btype == "pe":
        return _detect_pe_machine(real_path)
    return ""


def resolve_image_file(path: str, quiet: bool = False) -> str:
    """Resolve .efi → .dll → .debug chain. Returns ELF path."""
    real_path = os.path.realpath(path)
    btype = _detect_binary_type(real_path)

    if btype == "pe":
        # PE binary — extract embedded PDB/DLL path from strings
        for s in _extract_pe_strings(real_path):
            if re.search(r"\.(dll|debug|pdb)$", s):
                dll_path = s.strip()
                if os.path.isfile(dll_path):
                    if not quiet:
                        print(f"  Resolved .efi → {dll_path}", file=sys.stderr)
                    debug_file = re.sub(r"\.dll$", ".debug", dll_path)
                    if os.path.isfile(debug_file):
                        if not quiet:
                            print(f"  Resolved .dll → {debug_file}", file=sys.stderr)
                        return debug_file
                    return dll_path
                break

        # Try .debug alongside .efi
        base = re.sub(r"\.efi$", ".debug", path)
        if os.path.isfile(base):
            return base
        name = Path(path).stem
        candidate = os.path.join(os.path.dirname(path), f"{name}.debug")
        if os.path.isfile(candidate):
            return candidate

        if not quiet:
            print(f"  No debug ELF for PE image: {path} (will try .map)",
                  file=sys.stderr)
        return ""

    elif btype == "elf":
        return path
    else:
        print(f"Error: unrecognized file type: {path}", file=sys.stderr)
        sys.exit(1)


def register_image(spec: str, tc: Toolchain, quiet: bool = False) -> Image:
    """Parse 'file[:base]' spec and resolve to Image."""
    m = re.match(r"^(.+?):(0x[0-9a-fA-F]+)$", spec)
    if m:
        file_path, base_str = m.group(1), m.group(2)
        base = int(base_str, 16)
    else:
        file_path = spec
        base = None

    if not os.path.isfile(file_path):
        print(f"Error: file not found: {file_path}", file=sys.stderr)
        sys.exit(1)

    elf = resolve_image_file(file_path, quiet=quiet)
    name = re.sub(r"\.(debug|dll|efi|so)$", "", os.path.basename(file_path))

    # Look for .map file alongside the image
    map_file = ""
    for map_candidate in [
        re.sub(r"\.(efi|dll|debug|so)$", ".map", file_path),  # foo.map
        file_path + ".map",                                     # foo.efi.map
    ]:
        if os.path.isfile(map_candidate):
            map_file = map_candidate
            if not quiet:
                print(f"  Found map file: {map_file}", file=sys.stderr)
            break

    # Estimate size from last symbol
    size = 0x20000
    if elf:
        nm_out = _run([tc.nm, "-C", "-n", elf])
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1].lower() in ("t", "d", "b"):
                try:
                    size = int(parts[0], 16) + 0x1000
                except ValueError:
                    pass
    elif map_file:
        # Estimate size from map file addresses
        with open(map_file, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"(0x[0-9a-fA-F]+|[0-9a-fA-F]{16})\s+\S+$",
                              line)
                if m:
                    try:
                        addr = int(m.group(1), 16)
                        if addr > size:
                            size = addr + 0x1000
                    except ValueError:
                        pass

    # Parse preferred load address from map file for PE DWARF resolution
    pe_base = 0
    if map_file:
        with open(map_file, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"Preferred load address is\s+([0-9a-fA-F]+)",
                              line)
                if m:
                    pe_base = int(m.group(1), 16)
                    break

    return Image(elf=elf, base=base, name=name, size=size,
                 map_file=map_file, efi_path=file_path, pe_base=pe_base)


# ═══════════════════════════════════════════════════════════════
# Frame-pointer chain unwinding
#
# When the firmware prints no ordered `sNN` frame list (common on Dell x64
# RSODs — "Stack trace not available", only a raw stack dump), reconstruct an
# ordered backtrace by walking the frame-pointer chain through the dumped
# stack memory. Both AArch64 (x29) and x86-64 (rbp) use the same frame layout:
# [FP] = caller's saved FP, [FP+8] = caller's return address. Ported from the
# standalone rsod-decode's decoders/unwinding.py (the heavier DWARF/LLDB
# unwinders there need pip deps and stay out of this zero-dep variant).
# ═══════════════════════════════════════════════════════════════

def _first_present(regs: dict, names: Tuple[str, ...]) -> Optional[int]:
    """First register in @names that is present in @regs (value may be 0), else
    None. Lets a fault at address 0 be distinguished from a missing register."""
    for n in names:
        if n in regs:
            return regs[n]
    return None


# A stack window is at most a few MB; cap the reconstructed buffer so a stray
# low-address match in the noisy serial log (the dump regexes are loose) can't
# balloon it into hundreds of MB of zero-fill.
_MAX_STACK_SPAN = 4 * 1024 * 1024


def _build_stack_mem(pairs: List[Tuple[int, int]]) -> Tuple[int, bytes]:
    """Assemble (address, value) stack pairs into one contiguous little-endian
    buffer. Returns (base_address, bytes); gaps are zero-filled so a FP that
    lands in an unsampled hole simply reads 0 and stops the walk. If the pairs
    span more than _MAX_STACK_SPAN (stray outlier addresses), keep only the top
    window — the FP frames live near the top of a stack dump."""
    if not pairs:
        return 0, b""
    ordered = sorted(pairs)
    hi = ordered[-1][0]
    if hi - ordered[0][0] + 8 > _MAX_STACK_SPAN:
        lo_cut = hi - _MAX_STACK_SPAN + 8
        ordered = [p for p in ordered if p[0] >= lo_cut]
    base = ordered[0][0]
    end = ordered[-1][0] + 8
    buf = bytearray(end - base)
    for addr, val in ordered:
        off = addr - base
        if 0 <= off <= len(buf) - 8:
            struct.pack_into("<Q", buf, off, val & 0xFFFFFFFFFFFFFFFF)
    return base, bytes(buf)


def _walk_fp_chain(
    fp: int, first_ret: int, mem: bytes, base: int,
    monotonic: bool, max_frames: int = 48,
) -> List[int]:
    """Follow a saved-FP linked list through @mem, returning ordered return
    addresses (outermost-appended). @first_ret (LR / crash return) seeds frame
    0 when non-zero. @monotonic requires each saved FP to increase (x86-64
    stacks grow down, so a non-increasing rbp means a corrupt/looping chain)."""
    end = base + len(mem)
    out: List[int] = []
    if first_ret:
        out.append(first_ret)
    cur = fp
    for _ in range(max_frames):
        if cur == 0 or cur < base or cur + 16 > end:
            break
        off = cur - base
        saved_fp = struct.unpack_from("<Q", mem, off)[0]
        saved_ret = struct.unpack_from("<Q", mem, off + 8)[0]
        if saved_ret == 0:
            break
        out.append(saved_ret)
        if monotonic and saved_fp <= cur:
            break
        cur = saved_fp
    return out


def recover_backtrace_via_fp(rsod: "RsodData") -> List[int]:
    """Ordered return-address PCs reconstructed from the frame-pointer chain,
    or [] when there is nothing to walk. Used only as a fallback when the RSOD
    carried no `sNN` frame list. Frame 0 is the faulting PC (ELR / RIP)."""
    base, mem = _build_stack_mem(rsod.stack_pairs)
    if not mem:
        return []
    regs = rsod.registers
    # The fault PC uses _first_present so a legitimate fault AT address 0 (a
    # NULL call/jump — a real crash site) is kept, not mistaken for "absent".
    if rsod.arch == "AARCH64":
        fp = regs.get("FP") or regs.get("X29") or 0
        lr = regs.get("LR") or 0
        fault = _first_present(regs, ("ELR",))
        monotonic = False           # AArch64 stack direction is ABI-defined but
                                    # we don't assume it; the bounds check guards.
    else:
        # Dell x64 RSODs abbreviate the register names (BP/IP, no R prefix);
        # accept both spellings.
        fp = regs.get("RBP") or regs.get("BP") or 0
        lr = 0                       # x86-64 has no link register; the first
                                    # return comes from walking rbp.
        fault = _first_present(regs, ("RIP", "IP"))
        monotonic = True
    if fp == 0:
        return []
    walked = _walk_fp_chain(fp, lr, mem, base, monotonic)
    # Frame 0 is the crash site (ELR/RIP). Drop ONLY the first walked frame if
    # it coincides with the fault PC (LR often equals it) — not all consecutive
    # duplicates, so genuine tight recursion survives.
    pcs: List[int] = []
    if fault is not None:
        pcs.append(fault)
    for i, pc in enumerate(walked):
        if i == 0 and pcs and pc == pcs[0]:
            continue
        pcs.append(pc)
    return pcs


def resolve_addr_to_image(addr: int, images: List[Image]) -> int:
    """Return index of image containing addr, or -1."""
    for i, img in enumerate(images):
        if img.base is not None and img.base <= addr < img.base + img.size:
            return i
    return -1


# ═══════════════════════════════════════════════════════════════
# Symbol resolution — single path (DRY)
# ═══════════════════════════════════════════════════════════════

# Per-ELF gdb backend cache
_gdb_backends: Dict[str, GdbBackend] = {}


def _get_gdb_backend(tc: Toolchain, elf: str) -> Optional[GdbBackend]:
    """Get or create a GdbBackend for an ELF file."""
    if not tc.use_gdb:
        return None
    if elf not in _gdb_backends:
        _gdb_backends[elf] = GdbBackend(tc.gdb, elf, tc.arch)
    return _gdb_backends[elf]


def _resolve_address_pe_dwarf(efi_path: str, offset_hex: str,
                              preferred_base: int) -> ResolveResult:
    """Resolve using llvm-addr2line on a PE/COFF .efi with embedded DWARF.

    llvm-addr2line handles PE/COFF DWARF but needs the full VA
    (preferred base + offset), not just the offset.
    """
    result = ResolveResult()
    llvm_a2l = shutil.which("llvm-addr2line")
    if not llvm_a2l:
        return result

    try:
        offset = int(offset_hex, 16)
    except ValueError:
        return result

    va_hex = f"0x{preferred_base + offset:x}"
    out = _run([llvm_a2l, "-C", "-f", "-i", "-p", "-e", efi_path, va_hex])
    if out and "??" not in out:
        lines = [l for l in out.strip().splitlines() if l.strip()]
        for i, line in enumerate(lines):
            m = re.match(r"^(.+?) at (.+)$", line)
            if m:
                func = m.group(1).strip()
                file_line = re.sub(r"\s*\(discriminator \d+\)$", "",
                                   m.group(2).strip())
            else:
                func = line.strip()
                file_line = ""

            if i == 0:
                result.func = func
                result.status = "resolved"
                if ":" in file_line:
                    parts = file_line.rsplit(":", 1)
                    result.file = parts[0]
                    try:
                        result.line = int(parts[1])
                    except ValueError:
                        pass
            else:
                result.inlines.append((func, file_line))
    return result


def _resolve_address_map(map_file: str, offset_hex: str) -> ResolveResult:
    """Resolve an offset using a linker .map file (function-level only).

    Parses lld-link /MAP output format:
      0001:00000070  main  0000000180001070  hello.o
    """
    result = ResolveResult()
    if not map_file or not os.path.isfile(map_file):
        return result

    try:
        offset = int(offset_hex, 16)
    except ValueError:
        return result

    # Parse map entries: (rva, name, object_file)
    entries: list[tuple[int, str, str]] = []
    with open(map_file, encoding="utf-8", errors="replace") as f:
        for line in f:
            # Match: " 0001:00000070  name  0000000180001070  lib:obj.o"
            m = re.match(
                r"\s+\w+:\w+\s+(\S+)\s+(0x[0-9a-fA-F]+|[0-9a-fA-F]{16})\s+(\S+)",
                line)
            if m:
                name = m.group(1)
                try:
                    rva = int(m.group(2), 16)
                except ValueError:
                    continue
                obj = m.group(3)
                entries.append((rva, name, obj))

    if not entries:
        return result

    # Parse preferred load address from map file
    preferred_base = 0
    with open(map_file, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"Preferred load address is\s+([0-9a-fA-F]+)", line)
            if m:
                preferred_base = int(m.group(1), 16)
                break

    entries.sort(key=lambda e: e[0])

    # The map file has absolute addresses (preferred_base + RVA).
    # The offset from RSOD resolution is relative to image base.
    # Try: offset + preferred_base (most common), then offset as-is.
    for addr in [offset + preferred_base, offset]:
        best_name = ""
        best_obj = ""
        for rva, name, obj in entries:
            if rva <= addr:
                best_name = name
                best_obj = obj
            else:
                break
        if best_name:
            result.func = best_name
            result.file = best_obj
            result.status = "map_symbol"
            return result

    return result


def resolve_address(tc: Toolchain, elf: str, offset_hex: str,
                    map_file: str = "", efi_path: str = "",
                    pe_base: int = 0) -> ResolveResult:
    """Resolve an offset to function/file/line.

    Resolution chain:
      1. gdb/addr2line on ELF (DWARF debug symbols)
      2. llvm-addr2line on .efi (PE/COFF with embedded DWARF)
      3. .map file (function names only, no line numbers)
    """
    if offset_hex.startswith("0x-") or offset_hex.startswith("-"):
        return ResolveResult()

    if elf and not elf.endswith(".pdb"):
        # Prefer gdb backend (ELF/DWARF only, not PDB)
        gdb = _get_gdb_backend(tc, elf)
        if gdb:
            result = gdb.resolve(offset_hex)
            if result.status != "unknown":
                return result

        result = _resolve_address_binutils(tc, elf, offset_hex)
        if result.status != "unknown":
            return result

    # Try llvm-addr2line on PE/COFF .efi with embedded DWARF
    if efi_path and efi_path.endswith(".efi"):
        result = _resolve_address_pe_dwarf(efi_path, offset_hex, pe_base)
        if result.status != "unknown":
            return result

    # Fall back to .map file (function-level only, no line numbers)
    if map_file:
        return _resolve_address_map(map_file, offset_hex)

    return ResolveResult()


def _resolve_address_binutils(tc: Toolchain, elf: str, offset_hex: str) -> ResolveResult:
    """Fallback: resolve via addr2line → nm → nm -D."""
    result = ResolveResult()

    # 1. addr2line (DWARF)
    out = _run([tc.addr2line, "-C", "-f", "-i", "-p", "-e", elf, offset_hex])
    if out and "??" not in out:
        lines = [l for l in out.strip().splitlines() if l.strip()]
        for i, line in enumerate(lines):
            m = re.match(r"^(.+?) at (.+)$", line)
            if m:
                func = m.group(1).strip()
                file_line = re.sub(r"\s*\(discriminator \d+\)$", "", m.group(2).strip())
            else:
                func = line.strip()
                file_line = ""

            if i == 0:
                result.func = func
                result.status = "resolved"
                if ":" in file_line:
                    parts = file_line.rsplit(":", 1)
                    result.file = parts[0]
                    try:
                        result.line = int(parts[1])
                    except ValueError:
                        pass
            else:
                result.inlines.append((func, file_line))
        return result

    # 2. Static symbols (nm)
    offset = int(offset_hex, 16)
    sym = _find_nearest_symbol(tc.nm, elf, offset, "")
    if sym:
        result.func = sym
        result.status = "symbol_only"
        return result

    # 3. Dynamic symbols (nm -D) — stripped binaries
    sym = _find_nearest_symbol(tc.nm, elf, offset, "-D")
    if sym:
        result.func = sym
        result.status = "dynamic_symbol"
        return result

    return result


_nm_cache: Dict[Tuple[str, str, str], List[Tuple[int, str]]] = {}

def _get_nm_symbols(nm_cmd: str, elf: str, flags: str) -> List[Tuple[int, str]]:
    """Get sorted symbol list, cached per ELF+flags."""
    key = (nm_cmd, elf, flags)
    if key not in _nm_cache:
        args = [nm_cmd] + ([flags] if flags else []) + ["-C", "-n", elf]
        out = _run(args)
        symbols: list[tuple[int, str]] = []
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1].lower() == "t":
                try:
                    symbols.append((int(parts[0], 16), parts[2]))
                except ValueError:
                    pass
        _nm_cache[key] = symbols
    return _nm_cache[key]


def _find_nearest_symbol(nm_cmd: str, elf: str, target: int, flags: str) -> str:
    """Find nearest symbol at or below target offset."""
    symbols = _get_nm_symbols(nm_cmd, elf, flags)
    best_name, best_addr = "", 0

    for addr, name in symbols:
        if addr <= target and addr > best_addr:
            best_addr = addr
            best_name = name
        if addr > target:
            break

    if best_name:
        return f"{best_name}+0x{target - best_addr:x}"
    return ""


# ═══════════════════════════════════════════════════════════════
# RSOD text parsing
# ═══════════════════════════════════════════════════════════════

# ── Exception diagnosis ───────────────────────────────────────

# X64 exception vector → (name, description)
_X64_EXCEPTIONS: Dict[int, Tuple[str, str]] = {
    0x00: ("#DE", "Divide-by-zero"),
    0x01: ("#DB", "Debug exception"),
    0x03: ("#BP", "Breakpoint (INT3)"),
    0x04: ("#OF", "Overflow"),
    0x05: ("#BR", "Bound range exceeded"),
    0x06: ("#UD", "Invalid opcode — possible __builtin_trap(), stack corruption, or jump to bad address"),
    0x08: ("#DF", "Double fault — exception during exception handling"),
    0x0A: ("#TS", "Invalid TSS"),
    0x0B: ("#NP", "Segment not present"),
    0x0C: ("#SS", "Stack-segment fault"),
    0x0D: ("#GP", "General protection fault — bad pointer alignment, privilege violation, or invalid address"),
    0x0E: ("#PF", "Page fault"),
    0x10: ("#MF", "x87 floating-point exception"),
    0x11: ("#AC", "Alignment check"),
    0x12: ("#MC", "Machine check"),
    0x13: ("#XM", "SIMD floating-point exception"),
}

# AARCH64 ESR EC → description
_AARCH64_EC: Dict[int, str] = {
    0x00: "Unknown reason",
    0x01: "Trapped WFI/WFE",
    0x0E: "Illegal execution state",
    0x15: "SVC in AArch64",
    0x18: "Trapped MSR/MRS/system instruction",
    0x20: "Instruction abort from lower EL",
    0x21: "Instruction abort from same EL — jumped to unmapped/non-executable address",
    0x22: "PC alignment fault",
    0x24: "Data abort from lower EL",
    0x25: "Data abort from same EL",
    0x26: "SP alignment fault",
    0x2C: "Trapped FP exception",
    0x30: "Breakpoint from lower EL",
    0x31: "Breakpoint from same EL",
    0x32: "Software step from lower EL",
    0x33: "Software step from same EL",
    0x34: "Watchpoint from lower EL",
    0x35: "Watchpoint from same EL",
    0x38: "BKPT in AArch32",
    0x3C: "BRK in AArch64",
}

# AARCH64 ESR ISS Data/Instruction Fault Status Code (ISS[5:0]) → meaning.
# For a data/instruction abort this says *why* the access faulted (an
# unmapped page is a translation fault; a write to read-only is a permission
# fault; etc.) — far more actionable than "Data abort" alone.
_AARCH64_DFSC: Dict[int, str] = {
    0x00: "Address size fault, level 0", 0x01: "Address size fault, level 1",
    0x02: "Address size fault, level 2", 0x03: "Address size fault, level 3",
    0x04: "Translation fault, level 0",  0x05: "Translation fault, level 1",
    0x06: "Translation fault, level 2",  0x07: "Translation fault, level 3",
    0x09: "Access flag fault, level 1",  0x0A: "Access flag fault, level 2",
    0x0B: "Access flag fault, level 3",  0x0D: "Permission fault, level 1",
    0x0E: "Permission fault, level 2",   0x0F: "Permission fault, level 3",
    0x10: "Synchronous external abort",  0x21: "Alignment fault",
}


def _diagnose_fault_addr(addr_str: str) -> str:
    """Interpret a fault address (CR2 or FAR) into a human description."""
    if not addr_str:
        return ""
    try:
        val = int(addr_str, 16)
    except ValueError:
        return ""
    if val == 0:
        return "NULL pointer dereference"
    if val < 0x1000:
        return f"Near-NULL dereference (offset 0x{val:x} from NULL — likely struct member access via NULL pointer)"
    if val >= 0xFFFF000000000000:
        return "Access to kernel/non-canonical address"
    return ""


def _diagnose_x64(data: RsodData) -> str:
    """Produce a human-readable cause for an X64 exception."""
    parts: List[str] = []

    # Parse vector number from exception type like "0e(#PF)"
    m = re.match(r"([0-9a-fA-F]+)\(", data.exception_type)
    if m:
        vec = int(m.group(1), 16)
        info = _X64_EXCEPTIONS.get(vec)
        if info:
            parts.append(info[1])

        # Page fault specifics
        if vec == 0x0E:
            addr_diag = _diagnose_fault_addr(data.fault_addr)
            if addr_diag:
                parts.append(addr_diag)
            elif data.exception_data:
                exc_data = int(data.exception_data, 16)
                if exc_data & 0x10:  # instruction fetch
                    parts.append("Instruction fetch from unmapped page")
                elif exc_data & 0x02:  # write
                    parts.append("Write to unmapped/read-only page")
                else:
                    parts.append("Read from unmapped page")

        # #UD with CR2=0 or near-zero
        elif vec == 0x06:
            addr_diag = _diagnose_fault_addr(data.fault_addr)
            if addr_diag:
                parts.append(addr_diag)

        # #GP
        elif vec == 0x0D:
            addr_diag = _diagnose_fault_addr(data.fault_addr)
            if addr_diag:
                parts.append(addr_diag)

    return " — ".join(parts) if parts else ""


def _diagnose_aarch64(data: RsodData) -> str:
    """Produce a human-readable cause for an AARCH64 exception."""
    parts: List[str] = []

    # Parse EC from ESR decode: "EC 0x25  IL 0x1  ISS 0x00000047"
    if data.esr_decode:
        m = re.search(r"EC 0x([0-9a-fA-F]+)", data.esr_decode)
        if m:
            ec = int(m.group(1), 16)
            desc = _AARCH64_EC.get(ec)
            if desc:
                parts.append(desc)

            # Data/instruction abort — decode the fault status code (why it
            # faulted) from ISS[5:0], then interpret FAR.
            if ec in (0x24, 0x25, 0x20, 0x21):
                mi = re.search(r"ISS 0x([0-9a-fA-F]+)", data.esr_decode)
                if mi:
                    dfsc = _AARCH64_DFSC.get(int(mi.group(1), 16) & 0x3F)
                    if dfsc:
                        parts.append(dfsc)
                addr_diag = _diagnose_fault_addr(data.fault_addr)
                if addr_diag:
                    parts.append(addr_diag)

    if data.abort_desc and not parts:
        parts.append(data.abort_desc)

    return " — ".join(parts) if parts else ""


def parse_rsod(text: str) -> RsodData:
    """Detect RSOD format from structural patterns, not specific strings.

    Detection is based on the register and stack trace data formats:
      X64 EDK2:  "REG  - VALUE" registers, "ImageBase=" image info
      AARCH64 EDK2: "REG 0xVALUE" registers, "PC 0x... (0xBASE+0xOFF)" stack
      Dell BIOS: "REG=VALUE" registers, "sNN ADDR Module.efi +OFFSET" stack
    """
    data = RsodData()

    # Format 1: X64 — "REG  - HEXVALUE" register format
    if re.search(r"^R[A-Z][A-Z0-9]*\s+-\s+[0-9a-fA-F]{8,16}", text, re.M):
        data.arch = "X64"
        _parse_x64(text, data)
    # Format 2: Dell/vendor — "REG=HEXVALUE" registers or "sNN ADDR Module.efi
    # +OFFSET" stack. The sNN frames and the "--> PC/RIP" marker may be indented,
    # and register values may carry a 0x prefix (e.g. "ELR=0x00000078...").
    elif re.search(r"^\s*s\d+\s+[0-9a-fA-F]+\s+\S+\.efi\s+\+", text, re.M) \
            or re.search(r"^\s*-->\s*(?:RIP|PC)\b", text, re.M) \
            or re.search(r"\b[A-Z]{1,4}\d*=\s*(?:0x)?[0-9a-fA-F]{8,}", text) \
            or re.search(r"LBRfr0\s", text):
        # Detect X64 vs AARCH64 from register names. Check AArch64-distinctive
        # names FIRST (X0-X30 / ELR / ESR / FAR / SPSR) — SP= and BP= are shared
        # with x64 and must not decide the arch on their own.
        if re.search(r"\b(?:X\d+|ELR|ESR|FAR|SPSR)=|Synchronous exception", text):
            data.arch = "AARCH64"
        elif re.search(r"\bR?[ABCD]X=|\bR?IP=|\bR?SI=|\bR?DI=|-->RIP\s|LBRfr0\s", text):
            data.arch = "X64"
        else:
            data.arch = "AARCH64"
        _parse_dell_bios(text, data)  # parser handles both X64 and AARCH64
    # Format 3: EDK2 AARCH64 — "REG 0xVALUE" registers or "PC 0x..." stack
    elif re.search(r"^\s*X\d+\s+0x|ELR\s+0x|^PC\s+0x", text, re.M):
        data.arch = "AARCH64"
        _parse_aarch64(text, data)

    return data


def _parse_x64(text: str, data: RsodData):
    m = re.search(r"!!!! X64 Exception Type - ([^!]+)", text)
    if m:
        data.exception_type = m.group(1).strip()

    m = re.search(r"ExceptionData - ([0-9a-fA-F]+)", text)
    if m:
        data.exception_data = f"0x{m.group(1)}"

    m = re.search(r"^RIP  - ([0-9a-fA-F]+)", text, re.M)
    if m:
        data.fault_pc = f"0x{m.group(1)}"
        data.stack_pcs.append(int(m.group(1), 16))

    m = re.search(r"ImageBase=([0-9a-fA-F]+)", text)
    if m:
        data.parsed_base = int(m.group(1), 16)

    m = re.search(r"CR2 - ([0-9a-fA-F]+)", text)
    if m:
        data.fault_addr = f"0x{m.group(1)}"

    # Registers: "REG  - VALUE"
    for m in re.finditer(r"([A-Z][A-Z0-9]+)\s+-\s+([0-9a-fA-F]{8,16})", text):
        data.registers[m.group(1)] = int(m.group(2), 16)

    # Stack dump: lines of hex qwords
    for m in re.finditer(r"^\s*>?\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{16}\s*)+)", text, re.M):
        row_addr = int(m.group(1), 16)
        for i, qw in enumerate(m.group(2).split()):
            if len(qw) >= 8:
                val = int(qw, 16)
                data.stack_qwords.append(val)
                data.stack_pairs.append((row_addr + i * 8, val))

    data.cause = _diagnose_x64(data)


def _parse_aarch64(text: str, data: RsodData):
    m = re.search(r"(\w+) Exception at (0x[0-9a-fA-F]+)", text)
    if m:
        data.exception_type = f"{m.group(1)} Exception at {m.group(2)}"
        data.fault_pc = m.group(2)

    m = re.search(r"ELR (0x[0-9a-fA-F]+)", text)
    if m:
        data.fault_pc = m.group(1)

    m = re.search(r"FAR (0x[0-9a-fA-F]+)", text)
    if m:
        data.fault_addr = m.group(1)

    m = re.search(r"ESR : (EC 0x[0-9a-fA-F]+\s+IL 0x[0-9a-fA-F]+\s+ISS 0x[0-9a-fA-F]+)", text)
    if m:
        data.esr_decode = m.group(1)

    m = re.search(r"((?:Data|Instruction) abort: .+)", text)
    if m:
        data.abort_desc = m.group(1)

    # Stack trace PCs: "PC 0xADDR (0xBASE+0xOFFSET) [N] name.dll"
    for m in re.finditer(r"^PC (0x[0-9a-fA-F]+)\s+\((0x[0-9a-fA-F]+)\+(0x[0-9a-fA-F]+)\)\s+\[\s*\d+\]\s+(\S+)", text, re.M):
        pc = int(m.group(1), 16)
        base = int(m.group(2), 16)
        offset = int(m.group(3), 16)
        mod_name = re.sub(r"\.dll$", "", m.group(4))
        data.stack_pcs.append(pc)
        data.stack_frame_info.append((pc, mod_name, base, offset))
        data.module_bases[mod_name] = base
        if data.parsed_base is None:
            data.parsed_base = base

    if not data.stack_pcs and data.fault_pc:
        data.stack_pcs.append(int(data.fault_pc, 16))

    # Registers
    for m in re.finditer(r"(X\d+|FP|LR|SP|ELR|SPSR|FPSR|ESR|FAR)\s+0x([0-9a-fA-F]+)", text):
        data.registers[m.group(1)] = int(m.group(2), 16)

    # Stack dump
    for m in re.finditer(r"^\s*>?\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{16}\s*)+)", text, re.M):
        row_addr = int(m.group(1), 16)
        for i, qw in enumerate(m.group(2).split()):
            if len(qw) >= 8:
                val = int(qw, 16)
                data.stack_qwords.append(val)
                data.stack_pairs.append((row_addr + i * 8, val))

    data.cause = _diagnose_aarch64(data)


def _parse_dell_bios(text: str, data: RsodData):
    """Parse Dell/vendor BIOS RSOD format (X64 and AARCH64).

    Detects from structural patterns (REG=VALUE, sNN stack frames) rather
    than specific strings. Handles any exception type or syndrome text.

    Formats:
        X64:
            Type: Invalid opcode (06) Source: Software (UEFI0004) on BSP
            AX=VALUE BX=VALUE IP=VALUE ...
            -->RIP ADDR Description
            Stack Dump:
              ADDR  QWORD ...

        AARCH64:
            Type: <exception type>, Syndrome:<description>
            REG=VALUE    REG=VALUE    ...
            --> PC ADDR Description
            s00 ADDR Module.efi +OFFSET
    """
    # Exception type — grab any "Type:" line
    m = re.search(r"Type:\s*(.+)", text)
    if m:
        data.exception_type = m.group(1).strip()

    # Syndrome/description — extract whatever follows "Syndrome:"
    m = re.search(r"Syndrome:\s*(.+)", text)
    if m:
        data.abort_desc = m.group(1).strip()

    # Also capture the "--> PC/RIP ADDR Description" line if present
    m = re.search(r"-->\s*(?:PC|RIP)\s+[0-9a-fA-F]+\s+(.*)", text)
    if m and m.group(1).strip() and not data.abort_desc:
        data.abort_desc = m.group(1).strip()

    # Registers: any "NAME=HEXVALUE" pair (generic — handles unknown fields).
    # The value may carry a 0x prefix (e.g. "ELR=0x00000078262A3B3C").
    for m in re.finditer(r"([A-Za-z][A-Za-z0-9_]*)=(?:0x)?([0-9a-fA-F]{2,16})(?:\s|$|,)", text):
        reg = m.group(1)
        try:
            val = int(m.group(2), 16)
            data.registers[reg] = val
        except ValueError:
            pass

    # Extract fault PC from known register names (X64 + AARCH64)
    for reg in ("IP", "RIP", "PC", "ELR"):
        if reg in data.registers:
            data.fault_pc = f"0x{data.registers[reg]:x}"
            break

    # Extract fault address
    if "CR2" in data.registers:
        data.fault_addr = f"0x{data.registers['CR2']:x}"
    elif "FAR" in data.registers:
        data.fault_addr = f"0x{data.registers['FAR']:x}"

    # ESR decode (AARCH64 only)
    if "ESR" in data.registers:
        esr = data.registers["ESR"]
        ec = (esr >> 26) & 0x3F
        il = (esr >> 25) & 0x1
        iss = esr & 0x1FFFFFF
        data.esr_decode = f"EC 0x{ec:02x}  IL 0x{il:x}  ISS 0x{iss:07x}"

    # Faulting PC: "-->RIP ADDR" or "--> PC ADDR" — prepend as frame #0
    m = re.search(r"-->\s*(?:RIP|PC|IP)\s+([0-9a-fA-F]+)", text)
    if m:
        fault_pc_val = int(m.group(1), 16)
        if fault_pc_val != 0:
            data.stack_pcs.append(fault_pc_val)

    # Stack trace: "sNN ADDR ModuleName +OFFSET" (any extension or none). The
    # frames may be indented (leading whitespace) in some serial captures.
    for m in re.finditer(r"^\s*s(\d+)\s+([0-9a-fA-F]+)\s+(\S+)\s+\+([0-9a-fA-F]+)", text, re.M):
        pc = int(m.group(2), 16)
        mod_name = re.sub(r"\.(efi|dll|debug)$", "", m.group(3))
        offset = int(m.group(4), 16)
        base = pc - offset
        data.stack_pcs.append(pc)
        data.stack_frame_info.append((pc, mod_name, base, offset))
        data.module_bases[mod_name] = base
        if data.parsed_base is None and mod_name.lower() != "dxecore" and mod_name.lower() != "shell":
            data.parsed_base = base

    # Stack dump: "ADDR  QWORD ..." — scan for return addresses
    # Dell X64 format often has no sNN frames, only a raw dump
    for m in re.finditer(
            r"^\s*([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{16})\s", text, re.M):
        row_addr = int(m.group(1), 16)
        qw = int(m.group(2), 16)
        if qw != 0:
            data.stack_qwords.append(qw)
            data.stack_pairs.append((row_addr, qw))

    if not data.stack_pcs and data.fault_pc:
        data.stack_pcs.append(int(data.fault_pc, 16))

    # Use the correct diagnosis based on arch
    if data.arch == "X64":
        data.cause = _diagnose_x64(data)
    else:
        data.cause = _diagnose_aarch64(data)


# ═══════════════════════════════════════════════════════════════
# Output formatters
# ═══════════════════════════════════════════════════════════════

# ── Color helpers ─────────────────────────────────────────────

C_RED = "\033[0;31m"
C_GREEN = "\033[0;32m"
C_YELLOW = "\033[1;33m"
C_BLUE = "\033[0;34m"
C_CYAN = "\033[0;36m"
C_BOLD = "\033[1m"
C_DIM = "\033[2m"
C_NC = "\033[0m"


def red(s: str) -> str:
    return f"{C_RED}{s}{C_NC}"


def green(s: str) -> str:
    return f"{C_GREEN}{s}{C_NC}"


def yellow(s: str) -> str:
    return f"{C_YELLOW}{s}{C_NC}"


def blue(s: str) -> str:
    return f"{C_BLUE}{s}{C_NC}"


def cyan(s: str) -> str:
    return f"{C_CYAN}{s}{C_NC}"


def bold(s: str) -> str:
    return f"{C_BOLD}{s}{C_NC}"


def dim(s: str) -> str:
    return f"{C_DIM}{s}{C_NC}"


# ═══════════════════════════════════════════════════════════════
# Source root remapping
# ═══════════════════════════════════════════════════════════════

_remap_cache: Dict[str, str] = {}


def remap_source_path(dwarf_path: str, source_roots: List[str]) -> str:
    """Remap a DWARF build-machine path to a local path via --source-root dirs."""
    if not source_roots or not dwarf_path:
        return dwarf_path
    if dwarf_path in _remap_cache:
        return _remap_cache[dwarf_path]

    # If the file already exists locally, no remapping needed
    if os.path.isfile(dwarf_path):
        _remap_cache[dwarf_path] = dwarf_path
        return dwarf_path

    # Try progressively shorter suffixes of the DWARF path against each root
    parts = dwarf_path.replace("\\", "/").split("/")
    for root in source_roots:
        root = os.path.expanduser(root)
        for i in range(len(parts)):
            suffix = os.path.join(*parts[i:])
            candidate = os.path.join(root, suffix)
            if os.path.isfile(candidate):
                _remap_cache[dwarf_path] = candidate
                return candidate

    _remap_cache[dwarf_path] = dwarf_path
    return dwarf_path


# ═══════════════════════════════════════════════════════════════
# Link helpers
# ═══════════════════════════════════════════════════════════════

def _detect_github_info() -> Tuple[str, str]:
    """Auto-detect GitHub repo and commit from git. Returns (org/repo, sha) or ('', '')."""
    try:
        url = subprocess.check_output(
            ["git", "remote", "get-url", "origin"], text=True, stderr=subprocess.DEVNULL
        ).strip()
        sha = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
        # Extract org/repo from git@github.com:org/repo.git or https://github.com/org/repo.git
        m = re.search(r"github\.com[:/](.+?)(?:\.git)?$", url)
        if m:
            return m.group(1), sha
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return "", ""


def github_link(filepath: str, line: int, repo: str, commit: str, source_roots: List[str]) -> str:
    """Generate a GitHub link for a file:line. Returns URL or empty string."""
    if not repo or not filepath:
        return ""
    # Try to make the path relative to any source root or cwd
    rel = filepath
    for root in source_roots + [os.getcwd()]:
        root = os.path.expanduser(root)
        try:
            rel = os.path.relpath(filepath, root)
            if not rel.startswith(".."):
                break
        except ValueError:
            continue
    if rel.startswith(".."):
        # Try suffix matching — find the shortest suffix that looks like a repo path
        parts = filepath.replace("\\", "/").split("/")
        for i in range(len(parts)):
            suffix = "/".join(parts[i:])
            if not suffix.startswith("/") and not suffix.startswith(".."):
                rel = suffix
                break
    rel = rel.replace("\\", "/")
    url = f"https://github.com/{repo}/blob/{commit}/{rel}"
    if line:
        url += f"#L{line}"
    return url


def osc8_link(display: str, url: str) -> str:
    """Wrap display text in an OSC 8 terminal hyperlink."""
    if not url or not sys.stdout.isatty():
        return display
    return f"\033]8;;{url}\033\\{display}\033]8;;\033\\"


def file_url(filepath: str, line: int = 0) -> str:
    """Generate a file:// URI for a local file."""
    if not filepath or not os.path.isfile(filepath):
        return ""
    abspath = os.path.abspath(filepath)
    # file:// URI — VS Code and most terminals support this
    if sys.platform == "win32":
        uri = "file:///" + abspath.replace("\\", "/")
    else:
        uri = "file://" + abspath
    if line > 0:
        uri += f"#L{line}"
    return uri


# ═══════════════════════════════════════════════════════════════
# Path display helpers
# ═══════════════════════════════════════════════════════════════

def shorten_path(path: str) -> str:
    """Shorten an absolute path to a project-relative form for display."""
    # Try *Pkg/ (EDK2 convention)
    m = re.search(r"([A-Za-z]+Pkg/.+)", path)
    if m:
        return m.group(1)
    # Try common project-relative directories
    m = re.search(r"((?:Test|Library|Application|Include|Source|src)/.+)", path)
    if m:
        return m.group(1)
    # Fallback: last 3 path components
    parts = path.replace("\\", "/").split("/")
    if len(parts) > 3:
        return "/".join(parts[-3:])
    return path


def _clickable_loc(filepath: str, line: int) -> str:
    """Format a file:line as a clickable link (OSC 8 in terminals)."""
    short = f"{shorten_path(filepath)}:{line}" if line else shorten_path(filepath)
    url = file_url(filepath, line)
    if url:
        return blue(osc8_link(short, url))
    return blue(short)


def emit_human(
    arch: str, images: List[Image], rsod: RsodData,
    frames: List[StackFrame], reg_annots: List[RegAnnotation],
    stack_scan: List[RegAnnotation], tc: Toolchain,
    detail: bool = False
) -> None:
    title = images[0].name if images else "RSOD"
    print(bold(f"RSOD Decoder — {title} ({arch})"))
    if detail:
        for i, img in enumerate(images):
            base_str = f"  base=0x{img.base:x}" if img.base is not None else ""
            print(dim(f"Image {i}: {img.name}  {img.elf}{base_str}"))
    print()

    # Crash summary — the "what happened at a glance" block
    if rsod.cause:
        print(f"{bold('Cause:')} {red(rsod.cause)}")
    elif rsod.exception_type:
        print(f"{bold('Exception:')} {red(rsod.exception_type)}")

    if rsod.fault_addr:
        print(f"  Fault address: {rsod.fault_addr}")

    # Crash location one-liner from first resolved frame
    first_resolved = next((sf for sf in (frames or []) if sf.resolve.status == "resolved"), None)
    if first_resolved:
        f0 = first_resolved.resolve
        if f0.file and f0.line:
            print(f"  {bold('Location:')} {green(f0.func)}  {_clickable_loc(f0.file, f0.line)}")
        else:
            print(f"  {bold('Location:')} {green(f0.func)}")

    if detail and rsod.exception_type:
        print(f"  Exception: {rsod.exception_type}")
        if rsod.exception_data:
            print(f"  ExceptionData: {rsod.exception_data}")
        if rsod.esr_decode:
            print(f"  {rsod.esr_decode}")
        if rsod.abort_desc:
            print(f"  {red(rsod.abort_desc)}")
    print()

    if frames:
        _hdr = "Stack trace:"
        if rsod.recovered_via_fp:
            _hdr += "  (recovered via frame-pointer chain — order is heuristic)"
        print(bold(_hdr) + "\n")
        for sf in frames:
            _emit_frame(sf, images, len(images) > 1)
            if detail and sf.frame == 0 and sf.image_idx >= 0:
                _emit_disasm(tc, images[sf.image_idx].elf, sf.offset)
                if not tc.use_gdb:
                    r = sf.resolve
                    if r.file and r.line:
                        _emit_source(r.file, r.line)

    # Deduplicate: skip register annotations that point to addresses already in the stack trace
    stack_addrs: set[int] = {sf.addr for sf in frames} if frames else set()
    unique_reg_annots = [ra for ra in reg_annots if ra.addr not in stack_addrs]

    if unique_reg_annots:
        print(f"\n{bold('Registers with code pointers:')}\n")
        for ra in unique_reg_annots:
            r = ra.resolve
            if r.file and r.line:
                print(f"  {bold(f'{ra.reg:<4}')} {green(f'{r.func:<30}')} {_clickable_loc(r.file, r.line)} {dim(f'[0x{ra.addr:x}]')}")
            else:
                print(f"  {bold(f'{ra.reg:<4}')} {cyan(f'{r.func:<30}')} {dim(f'[0x{ra.addr:x}]')}")

    # Full register dump
    if rsod.registers:
        print(f"\n{bold('Registers:')}\n")
        # Group registers into rows of 4
        reg_items = list(rsod.registers.items())
        for i in range(0, len(reg_items), 4):
            row = reg_items[i:i+4]
            parts: list[str] = []
            for reg, val in row:
                parts.append(f"  {dim(f'{reg:>4}')} {f'0x{val:016x}'}")
            print("".join(parts))

    if stack_scan and detail:
        print(f"\n{bold('Stack scan (possible return addresses):')}\n")
        for ra in stack_scan:
            r = ra.resolve
            if r.file and r.line:
                print(f"  {cyan(f'{r.func:<30}')} {_clickable_loc(r.file, r.line)} {dim(f'[0x{ra.addr:x}] (stack scan)')}")
            else:
                print(f"  {cyan(f'{r.func:<30}')} {dim(f'[0x{ra.addr:x}] (stack scan)')}")


def _emit_frame(sf: StackFrame, images: List[Image], multi: bool):
    r = sf.resolve
    prefix = dim(f"#{sf.frame:<2}")

    if sf.image_idx < 0:
        if sf.module_name:
            label = f"{sf.module_name}+0x{sf.offset:x}"
            print(f"  {prefix} {yellow(f'{label:<30}')} {dim(f'[0x{sf.addr:x}]  (no debug image)')}")
        else:
            if sf.frame == 0:
                print(f"  {prefix} {red(f'0x{sf.addr:x}  (faulting PC — invalid address)')}")
            else:
                print(f"  {prefix} {red(f'0x{sf.addr:x}  (outside all known images)')}")
        return

    img_tag = f" {dim(f'<{images[sf.image_idx].name}>')}" if multi else ""
    off_hex = f"0x{sf.offset:x}"
    addr_info = dim(f"[0x{sf.addr:x} + {off_hex}]")

    if r.status == "resolved":
        if r.file and r.line:
            print(f"  {prefix} {green(f'{r.func:<30}')} {_clickable_loc(r.file, r.line)} {addr_info}{img_tag}")
        else:
            print(f"  {prefix} {green(f'{r.func:<30}')} {addr_info}{img_tag}")
        for ifunc, ifline in r.inlines:
            if ifline and ":" in ifline:
                ifile, iline_s = ifline.rsplit(":", 1)
                try:
                    iline_n = int(iline_s)
                except ValueError:
                    iline_n = 0
                print(f"  {dim('    ')} {cyan(f'{ifunc:<30}')} {_clickable_loc(ifile, iline_n)} {dim('(inlined)')}")
            else:
                print(f"  {dim('    ')} {cyan(f'{ifunc:<30}')} {dim('(inlined)')}")
    elif r.status in ("symbol_only", "dynamic_symbol", "map_symbol"):
        print(f"  {prefix} {cyan(f'{r.func:<30}')} {addr_info}{img_tag}")
    else:
        unknown = "???"
        print(f"  {prefix} {yellow(f'{unknown:<30}')} {addr_info}")


def _emit_disasm(tc: Toolchain, elf: str, offset: int):
    # Try gdb backend first (interleaves source with disassembly)
    gdb = _get_gdb_backend(tc, elf)
    if gdb:
        lines = gdb.disassemble(offset)
        if lines:
            print(f"\n{C_BOLD}Disassembly:{C_NC}\n")
            print("\n".join(lines))
        return

    if not _which(tc.objdump):
        return

    # Find function start from cached nm symbols
    func_start = offset
    symbols = _get_nm_symbols(tc.nm, elf, "")
    best = 0
    for a, _ in symbols:
        if a <= offset and a > best:
            best = a
        if a > offset:
            break
    if best > 0:
        func_start = best

    stop = offset + 24
    out = _run([
        tc.objdump, "-d", "-C", "--no-show-raw-insn",
        f"--start-address={func_start}", f"--stop-address={stop}", elf
    ])
    if not out:
        return

    fault_hex = f"{offset:x}"
    lines: list[str] = []
    for line in out.splitlines():
        m = re.match(r"^\s+([0-9a-f]+):", line)
        if m:
            if m.group(1) == fault_hex:
                lines.append(f"  {C_RED}>>>{C_NC} {line}")
            else:
                lines.append(f"      {line}")

    if lines:
        print(f"\n{C_BOLD}Disassembly:{C_NC}\n")
        print("\n".join(lines))


def _emit_source(filepath: str, line: int, context: int = 2):
    if not filepath or not line or not os.path.isfile(filepath):
        return

    start = max(1, line - context)
    stop = line + context

    try:
        with open(filepath) as f:
            all_lines = f.readlines()
    except OSError:
        return

    print(f"\n{C_BOLD}Source:{C_NC} {shorten_path(filepath)}\n")
    for n in range(start, min(stop + 1, len(all_lines) + 1)):
        content = all_lines[n - 1].rstrip()
        if n == line:
            print(f"  {C_RED}>>>{n:4d}{C_NC} {content}")
        else:
            print(f"  {C_DIM}   {n:4d}{C_NC} {content}")


def emit_json(
    arch: str, images: List[Image], rsod: RsodData,
    frames: List[StackFrame], reg_annots: List[RegAnnotation],
    stack_scan: List[RegAnnotation]
) -> None:
    def _frame_dict(sf: StackFrame) -> Dict[str, object]:
        if sf.image_idx >= 0:
            image_name = images[sf.image_idx].name
        elif sf.module_name:
            image_name = sf.module_name
        else:
            image_name = None
        return {
            "frame": sf.frame,
            "addr": f"0x{sf.addr:x}",
            "offset": f"0x{sf.offset:x}" if sf.image_idx >= 0 or sf.module_name else None,
            "function": sf.resolve.func if sf.resolve.func != "???" else None,
            "file": sf.resolve.file or None,
            "line": sf.resolve.line or 0,
            "image": image_name,
            "status": "no_debug_image" if sf.module_name and sf.image_idx < 0 else sf.resolve.status,
        }

    def _annot_dict(ra: RegAnnotation) -> Dict[str, object]:
        return {
            "reg": ra.reg,
            "addr": f"0x{ra.addr:x}",
            "function": ra.resolve.func,
            "file": ra.resolve.file or None,
            "line": ra.resolve.line or 0,
            "status": ra.resolve.status,
        }

    data: dict[str, Any] = {
        "arch": arch,
        "exception": {
            "type": rsod.exception_type,
            "data": rsod.exception_data,
            "fault_pc": rsod.fault_pc,
            "fault_addr": rsod.fault_addr,
            "esr": rsod.esr_decode,
            "abort": rsod.abort_desc,
            "cause": rsod.cause,
        },
        "images": [
            {"name": img.name, "base": f"0x{img.base:x}" if img.base is not None else None, "debug_elf": img.elf}
            for img in images
        ],
        "stack_trace": [_frame_dict(sf) for sf in frames],
        "registers": {k: f"0x{v:x}" for k, v in rsod.registers.items()},
        "backtrace_recovered_via_fp": rsod.recovered_via_fp,
        "register_annotations": [_annot_dict(ra) for ra in reg_annots],
        "stack_scan": [_annot_dict(ra) for ra in stack_scan],
    }
    print(json.dumps(data, indent=2))


def emit_markdown(
    arch: str, images: List[Image], rsod: RsodData,
    frames: List[StackFrame], reg_annots: List[RegAnnotation],
    _stack_scan: List[RegAnnotation], tc: Toolchain,
    detail: bool, repo: str, commit: str, source_roots: List[str]
) -> None:
    """Emit a Markdown document with clickable links."""
    from datetime import datetime, timezone

    def _md_link(filepath: str, line: int) -> str:
        """Generate a markdown link for a file:line, with cross-repo safety."""
        if not filepath:
            return ""
        display = f"{shorten_path(filepath)}:{line}" if line else shorten_path(filepath)
        if repo and commit:
            url = github_link(filepath, line, repo, commit, source_roots)
            # Reject links that contain absolute path fragments (cross-repo files)
            if url and "/home/" not in url and "/builddir/" not in url and "/usr/" not in url:
                return f"[{display}]({url})"
        # Relative path link (works in VS Code markdown preview)
        rel = shorten_path(filepath)
        if line:
            return f"[{display}]({rel}#L{line})"
        return f"[{display}]({rel})"

    def _inline_link(ifline: str) -> str:
        """Parse an inline 'filepath:line' string and generate a link."""
        if not ifline or ":" not in ifline:
            return ifline or ""
        # Split on last colon (filepath may contain colons on Windows)
        file_part, line_part = ifline.rsplit(":", 1)
        try:
            line_num = int(line_part)
        except ValueError:
            return shorten_path(ifline)
        return _md_link(file_part, line_num)

    multi = len(images) > 1

    def _frame_row(sf: StackFrame, note: str = "") -> str:
        r = sf.resolve
        off_str = f"`+0x{sf.offset:x}`" if sf.image_idx >= 0 or sf.module_name else ""
        note_str = f" {note}" if note else ""
        img_name = ""
        if sf.image_idx >= 0:
            img_name = images[sf.image_idx].name
        elif sf.module_name:
            img_name = sf.module_name

        if sf.image_idx < 0 and sf.module_name:
            func = f"`{sf.module_name}+0x{sf.offset:x}`"
            loc = "*(no debug image)*"
        elif sf.image_idx < 0:
            func = "???"
            loc = ""
        else:
            func = f"`{r.func}`" if r.func != "???" else "???"
            loc = _md_link(r.file, r.line) if r.file and r.line else ""

        cols = [f"#{sf.frame}{note_str}", func, loc, off_str, f"`0x{sf.addr:x}`"]
        if multi:
            cols.append(img_name)
        return "| " + " | ".join(cols) + " |"

    # Header
    now = datetime.now(timezone.utc).astimezone()
    print(f"# RSOD Report — {images[0].name} ({arch})")
    print(f"\n*Generated: {now.strftime('%Y-%m-%d %H:%M:%S %Z')}*\n")

    # Exception summary
    if rsod.cause or rsod.exception_type:
        print("## Exception\n")
        if rsod.cause:
            print(f"> **Cause:** {rsod.cause}\n")
        if rsod.fault_addr:
            print(f"- **Fault address:** `{rsod.fault_addr}`")
        first_resolved = next((sf for sf in (frames or []) if sf.resolve.status == "resolved"), None)
        if first_resolved:
            f0 = first_resolved.resolve
            loc = _md_link(f0.file, f0.line) if f0.file and f0.line else ""
            print(f"- **Location:** `{f0.func}` — {loc}")
        if detail:
            if rsod.exception_type:
                print(f"- **Exception:** `{rsod.exception_type}`")
            if rsod.exception_data:
                print(f"- **ExceptionData:** `{rsod.exception_data}`")
            if rsod.esr_decode:
                print(f"- **ESR:** `{rsod.esr_decode}`")
            if rsod.abort_desc:
                print(f"- **Abort:** {rsod.abort_desc}")
        print()

    # Stack trace table — deduplicate consecutive identical frames
    if frames:
        print("## Stack Trace\n")
        if rsod.recovered_via_fp:
            print("*Recovered by walking the frame-pointer chain (no firmware "
                  "`sNN` frames) — frame order is heuristic.*\n")
        header = "| Frame | Function | Location | Offset | Address |"
        sep = "|-------|----------|----------|--------|---------|"
        if multi:
            header += " Image |"
            sep += "-------|"
        print(header)
        print(sep)

        prev_key = ""
        for sf in frames:
            r = sf.resolve
            key = f"{r.func}|{r.file}|{r.line}"
            if key == prev_key and r.status == "resolved":
                # Duplicate — skip but note
                continue
            prev_key = key
            print(_frame_row(sf))
            for ifunc, ifline in r.inlines:
                iloc = _inline_link(ifline)
                inline_cols = ["", f"↳ `{ifunc}`", iloc, "", "*(inlined)*"]
                if multi:
                    inline_cols.append("")
                print("| " + " | ".join(inline_cols) + " |")
        print()

    # Disassembly (detail mode)
    if detail and frames and frames[0].image_idx >= 0:
        gdb = _get_gdb_backend(tc, images[frames[0].image_idx].elf)
        if gdb:
            disasm_lines = gdb.disassemble(frames[0].offset)
            if disasm_lines:
                clean = [re.sub(r"\033\[[0-9;]*m", "", l) for l in disasm_lines]
                print("## Disassembly\n")
                print("```asm")
                for line in clean:
                    print(line)
                print("```\n")

    # Register annotations
    stack_addrs = {sf.addr for sf in frames}
    unique_regs = [ra for ra in reg_annots if ra.addr not in stack_addrs]
    if unique_regs:
        print("## Registers\n")
        print("| Register | Function | Location | Address |")
        print("|----------|----------|----------|---------|")
        for ra in unique_regs:
            r = ra.resolve
            loc = _md_link(r.file, r.line) if r.file and r.line else ""
            func = f"`{r.func}`" if r.func != "???" else "???"
            print(f"| {ra.reg} | {func} | {loc} | `0x{ra.addr:x}` |")
        print()

    # Images
    if detail:
        print("## Images\n")
        for img in images:
            base_str = f"`0x{img.base:x}`" if img.base is not None else "N/A"
            print(f"- **{img.name}**: {img.elf} (base: {base_str})")
        print()


def emit_dump(images: List[Image], tc: Toolchain):
    for img in images:
        print(f"Symbols: {img.elf} ({img.name})\n")
        out = _run([tc.nm, "-C", "-n", img.elf])
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1].lower() == "t":
                print(f"  0x{parts[0]}  {parts[2]}")
        print()


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

# ═══════════════════════════════════════════════════════════════
# OCR support
# ═══════════════════════════════════════════════════════════════

def _ocr_image(image_path: str) -> str:
    """Extract text from an RSOD photo using tesseract OCR.

    Uses tesseract in single-block mode (PSM 6) with a character whitelist
    tuned for RSOD output (hex digits, register names, etc.).
    Requires tesseract to be installed on the system.
    """
    if not os.path.isfile(image_path):
        print(f"Error: image not found: {image_path}", file=sys.stderr)
        sys.exit(1)

    if not _which("tesseract"):
        print("Error: tesseract not found on PATH", file=sys.stderr)
        is_win = sys.platform == "win32"
        if is_win:
            print("Install: download from https://github.com/UB-Mannheim/tesseract/wiki", file=sys.stderr)
        else:
            print("Install: sudo dnf install tesseract  (or apt install tesseract-ocr)", file=sys.stderr)
        sys.exit(1)

    # Run tesseract with:
    #   PSM 6: single uniform block of text
    #   OEM 3: default (LSTM + legacy)
    # The RSOD is monospace text on a solid background — PSM 6 works well.
    try:
        result = subprocess.run(
            ["tesseract", image_path, "stdout", "--psm", "6"],
            capture_output=True, text=True, timeout=30
        )
        text = result.stdout
    except subprocess.TimeoutExpired:
        print("Error: tesseract timed out", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print("Error: tesseract not found", file=sys.stderr)
        sys.exit(1)

    if not text.strip():
        print("Warning: OCR produced no text — the image may need better contrast", file=sys.stderr)
        return ""

    # Post-process common OCR errors in hex/register text
    text = _ocr_fixup(text)

    print(f"{dim(f'OCR extracted {len(text.splitlines())} lines from {image_path}')}", file=sys.stderr)
    return text


def _ocr_fixup(text: str) -> str:
    """Fix common OCR misreads in RSOD hex dump text.

    Tesseract often confuses these characters in monospace hex output:
        O ↔ 0,  l/I ↔ 1,  S ↔ 5,  G ↔ 6
        $ ↔ s (in stack frame numbers)
        % ↔ X (in register names)
        ¥ ↔ X (in register names)
        Z ↔ 2 (in hex values)
    We apply fixes based on structural context, not blindly.
    """

    def _fix_hex_value(m: re.Match[str]) -> str:
        """Fix OCR errors in a hex value string."""
        val: str = m.group(0)
        val = val.replace("O", "0").replace("o", "0")
        val = val.replace("l", "1").replace("I", "1")
        val = val.replace("S", "5").replace("G", "6")
        val = val.replace("Z", "2").replace("z", "2")
        val = val.replace("N", "0")
        return val

    lines: list[str] = []
    for line in text.splitlines():
        # --- Stack frame prefix fixes ---
        # OCR reads 's' as '8', '9', '$', or other digits in "sNN ADDR Module +OFF"
        # Pattern: starts with 1-3 digits/chars, then space, then hex address, then module.efi
        line = re.sub(r"^\$(\d{2})\s", r"s\1 ", line)
        # 800 → s00, 801 → s01, 901 → s01, etc. (digit-digit-digit before hex addr + .efi)
        line = re.sub(
            r"^[89](\d{2})\s+([0-9a-fA-F]{6,16}\s+\S+\.efi)",
            r"s\1 \2", line
        )
        # «300 → s00 (OCR garbage before digits)
        line = re.sub(
            r"^[^s\d]*(\d{2})\s+([0-9a-fA-F]{6,16}\s+\S+\.efi)",
            r"s\1 \2", line, count=1
        )
        # sO7 → s07, s1O → s10
        line = re.sub(r"^sO(\d)\s", r"s0\1 ", line)
        line = re.sub(r"^s(\d)O\s", lambda m: f"s{m.group(1)}0 ", line)

        # --- RIP/PC pointer fixes ---
        # ->RIP → -->RIP (missing first dash)
        line = re.sub(r"^->RIP\s", "-->RIP ", line)
        # ->PC → -->PC
        line = re.sub(r"^->PC\s", "-->PC ", line)
        # LBRfrO → LBRfr0, LBRtoO → LBRto0
        line = re.sub(r"^LBRfrO\s", "LBRfr0 ", line)
        line = re.sub(r"^LBRtoO\s", "LBRto0 ", line)

        # --- Register name fixes ---
        # %NN → XNN, ¥%NN → XNN, X%NN → XNN (AARCH64 register OCR)
        line = re.sub(r"[¥%]+(\d+)=", r"X\1=", line)
        line = re.sub(r"X%(\d+)=", r"X\1=", line)
        # RAK → RAX, RCK → RCX, RST → RSI, RBK → RBX (x86 register OCR)
        line = re.sub(r"\bRAK=", "RAX=", line)
        line = re.sub(r"\bRBK=", "RBX=", line)
        line = re.sub(r"\bRCK=", "RCX=", line)
        line = re.sub(r"\bRDK=", "RDX=", line)
        line = re.sub(r"\bRST=", "RSI=", line)
        # lags= → Flags=
        line = re.sub(r"\blags=", "Flags=", line)

        # --- Module name fixes ---
        # "Module .efi" → "Module.efi", "Module-efi" → "Module.efi"
        line = re.sub(r"(\S+)\s+\.efi", r"\1.efi", line)
        line = re.sub(r"(\w)-efi\b", r"\1.efi", line)
        line = re.sub(r"(\S+)-\.efi", r"\1.efi", line)

        # --- Misc cleanup ---
        line = re.sub(r"\s*§§\s*", " ", line)
        # Strip trailing OCR garbage after offset: "+035A90 (ox," → "+035A90"
        line = re.sub(r"(\+[0-9a-fA-FOolISGZzN]{4,8})\s*\(.*$", r"\1", line)
        # Strip trailing '&' or other single-char garbage
        line = re.sub(r"\s+[&@#|]$", "", line)

        # --- Hex value fixes in known contexts ---
        # Register values after '='
        line = re.sub(r"(?<==)[0-9a-fA-FOolISGZzN]{8,16}", _fix_hex_value, line)

        # Stack trace hex: address field after "sNN "
        if re.match(r"^s\d\d\s", line):
            line = re.sub(r"(?<=^s\d\d\s)[0-9a-fA-FOolISGZzN]{6,16}", _fix_hex_value, line)
        # Offset after '+'
        line = re.sub(r"(?<=\+)[0-9a-fA-FOolISGZzN]{4,8}", _fix_hex_value, line)

        # X64 register format: REG  - HEXVALUE
        line = re.sub(r"(?<=-\s)[0-9a-fA-FOolISGZzN]{8,16}", _fix_hex_value, line)

        # -->PC/-->RIP hex values
        line = re.sub(r"(?<=PC\s)[0-9a-fA-FOolISGZzN]{6,16}", _fix_hex_value, line)
        line = re.sub(r"(?<=RIP\s)[0-9a-fA-FOolISGZzN]{6,16}", _fix_hex_value, line)
        # LBR hex values
        line = re.sub(r"(?<=LBRfr0\s)[0-9a-fA-FOolISGZzN]{6,16}", _fix_hex_value, line)
        line = re.sub(r"(?<=LBRto0\s)[0-9a-fA-FOolISGZzN]{6,16}", _fix_hex_value, line)

        lines.append(line)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="UEFI RSOD Decoder — parse crash dumps and resolve to source locations.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  # Parse RSOD from serial log (compact output)
  rsod-decode.py --image IpmiTool.efi --file rsod_log.txt

  # Detailed output with disassembly and source context
  rsod-decode.py --image IpmiTool.debug --detail --file rsod.txt

  # Multi-image (AARCH64 cross-module stack trace)
  rsod-decode.py --image App.debug --image DxeCore.debug:0x47683000 --file rsod.txt

  # Markdown report with GitHub links
  rsod-decode.py --image App.debug --markdown --file rsod.txt > crash.md
  rsod-decode.py --image App.debug --markdown --repo org/repo --file rsod.txt

  # Remap build-machine paths to local source
  rsod-decode.py --image App.debug --source-root ~/projects/edk2 --file rsod.txt

  # Manual address decode
  rsod-decode.py --image App.debug --base 0x6A3C0000 --addr 0x6A3C02EB

  # JSON output / pipe from clipboard
  rsod-decode.py --image App.debug --json --file rsod.txt
  cat serial.log | rsod-decode.py --image App.debug

  # Dump all symbols
  rsod-decode.py --image App.debug --dump

For full documentation, run: python3 rsod-decode.py; pydoc3 rsod-decode
""",
    )
    parser.add_argument("--image", "--debug", action="append", dest="images", metavar="FILE[:BASE]",
                        help="EFI image (.debug, .dll, .so, .efi). Repeatable. Optional :BASE suffix.")
    parser.add_argument("--base", help="Default image base address (hex)")
    parser.add_argument("--arch", choices=["X64", "AARCH64", "IA32"], help="Override architecture")
    parser.add_argument("--file", dest="rsod_file", help="RSOD text file")
    parser.add_argument("--addr", nargs="+", metavar="HEX", help="Manual address list")
    parser.add_argument("--detail", action="store_true", help="Show disassembly, source context, and stack scan")
    parser.add_argument("--markdown", action="store_true", help="Markdown output with clickable links")
    parser.add_argument("--repo", metavar="ORG/REPO", help="GitHub repo for links (auto-detected from git)")
    parser.add_argument("--commit", metavar="SHA", help="Git commit for GitHub links (auto-detected)")
    parser.add_argument("--source-root", action="append", dest="source_roots", default=[], metavar="DIR",
                        help="Local source directory for path remapping (repeatable)")
    parser.add_argument("--ocr", metavar="IMAGE", help="OCR an RSOD photo (requires tesseract)")
    parser.add_argument("--json", action="store_true", help="JSON output")
    parser.add_argument("--dump", action="store_true", help="Dump all symbols")
    parser.add_argument("--fp-unwind", action="store_true", dest="fp_unwind",
                        help="Force a frame-pointer-chain backtrace even when "
                             "the firmware printed sNN frames (to compare or "
                             "override them)")

    args = parser.parse_args()

    if not args.images and not args.ocr and not args.rsod_file and not args.addr:
        parser.error("--image, --ocr, --file, or --addr is required")

    # Detect arch from first image (if provided)
    elf_arch = ""
    if args.images:
        first_file = re.sub(r":0x[0-9a-fA-F]+$", "", args.images[0], flags=re.IGNORECASE)
        elf_arch = detect_file_arch(first_file) if os.path.isfile(first_file) else ""

    # Read RSOD text
    rsod = RsodData()
    rsod_text = ""
    if args.ocr:
        rsod_text = _ocr_image(args.ocr)
    elif args.rsod_file:
        if not os.path.isfile(args.rsod_file):
            print(f"Error: file not found: {args.rsod_file}", file=sys.stderr)
            sys.exit(1)
        rsod_text = Path(args.rsod_file).read_text()
    elif not args.addr and not sys.stdin.isatty():
        rsod_text = sys.stdin.read()

    if rsod_text:
        rsod = parse_rsod(rsod_text)

    # Resolve architecture
    arch = args.arch or rsod.arch or elf_arch
    if not arch:
        print("Error: cannot detect arch, use --arch", file=sys.stderr)
        sys.exit(1)

    tc = Toolchain(arch)
    tc.check()

    # Register images
    images: List[Image] = []
    image_specs: list[str] = args.images or []
    for spec in image_specs:
        images.append(register_image(spec, tc, quiet=args.json))

    # Apply bases: RSOD-parsed module bases → parsed_base → --base
    default_base = int(args.base, 16) if args.base else None
    for i, img in enumerate(images):
        if img.base is None:
            if img.name in rsod.module_bases:
                img.base = rsod.module_bases[img.name]
            elif i == 0 and rsod.parsed_base is not None:
                img.base = rsod.parsed_base
            elif default_base is not None:
                img.base = default_base

    # Dump mode
    if args.dump:
        emit_dump(images, tc)
        return

    # Collect addresses.
    # A REAL firmware frame list populates stack_frame_info; when there are no
    # sNN frames the parsers still SEED stack_pcs with just the fault PC, so
    # "stack_pcs is non-empty" is NOT the same as "has a backtrace". Gate the
    # FP-chain recovery on the absence of a real frame list.
    have_real_frames = bool(rsod.stack_frame_info) or len(rsod.stack_pcs) > 1
    addrs = []
    recovered_via_fp = False
    if args.addr:
        addrs = [int(a, 16) for a in args.addr]
    elif args.fp_unwind:
        # Explicit override: reconstruct from the FP chain, ignoring any sNN
        # frames the firmware printed. Fall back to the seed if nothing walks.
        addrs = recover_backtrace_via_fp(rsod)
        recovered_via_fp = bool(addrs)
        if not addrs:
            addrs = rsod.stack_pcs
    elif have_real_frames:
        addrs = rsod.stack_pcs
    else:
        # No real frame list (at most the seeded fault PC) — reconstruct an
        # ordered backtrace by walking the frame-pointer chain through the raw
        # stack dump; keep the seed if the walk yields nothing.
        fp_pcs = recover_backtrace_via_fp(rsod)
        if fp_pcs:
            addrs = fp_pcs
            recovered_via_fp = True
        else:
            addrs = rsod.stack_pcs
    rsod.recovered_via_fp = recovered_via_fp

    if not addrs and not rsod_text:
        print("Error: no addresses to decode", file=sys.stderr)
        sys.exit(1)

    # ── Resolve everything ────────────────────────────────────

    # Build a lookup from PC → (module_name, base, offset) from RSOD text
    rsod_frame_map: Dict[int, Tuple[str, int, int]] = {}
    for pc, mod_name, base, offset in rsod.stack_frame_info:
        rsod_frame_map[pc] = (mod_name, base, offset)

    def _img_base(idx: int) -> int:
        """Get image base, guaranteed non-None (resolve_addr_to_image only matches when base is set)."""
        b = images[idx].base
        assert b is not None
        return b

    # Batch pre-resolve: collect all offsets per image, resolve in one gdb call each
    if tc.use_gdb:
        offsets_per_image: Dict[int, List[str]] = {}
        for addr in addrs:
            idx = resolve_addr_to_image(addr, images)
            if idx < 0 and len(images) == 1 and images[0].base is not None:
                off = addr - _img_base(0)
                if 0 <= off < images[0].size:
                    idx = 0
            if idx >= 0:
                off_hex = f"0x{addr - _img_base(idx):x}"
                offsets_per_image.setdefault(idx, []).append(off_hex)
        # Also pre-resolve register values
        for val in rsod.registers.values():
            if val == 0:
                continue
            idx = resolve_addr_to_image(val, images)
            if idx >= 0:
                off_hex = f"0x{val - _img_base(idx):x}"
                offsets_per_image.setdefault(idx, []).append(off_hex)
        for idx, offsets in offsets_per_image.items():
            gdb = _get_gdb_backend(tc, images[idx].elf)
            if gdb:
                gdb.resolve_batch(offsets)

    frames: List[StackFrame] = []
    for i, addr in enumerate(addrs):
        sf = StackFrame(frame=i, addr=addr)
        idx = resolve_addr_to_image(addr, images)
        if idx >= 0:
            sf.image_idx = idx
            sf.offset = addr - _img_base(idx)
            sf.resolve = resolve_address(tc, images[idx].elf, f"0x{sf.offset:x}", images[idx].map_file, images[idx].efi_path, images[idx].pe_base)
        elif addr in rsod_frame_map:
            mod_name, base, offset = rsod_frame_map[addr]
            sf.module_name = mod_name
            sf.offset = offset
        elif len(images) == 1 and images[0].base is not None:
            off = addr - _img_base(0)
            if 0 <= off < images[0].size:
                sf.image_idx = 0
                sf.offset = off
                sf.resolve = resolve_address(tc, images[0].elf, f"0x{sf.offset:x}", images[0].map_file, images[0].efi_path, images[0].pe_base)
        frames.append(sf)

    # Register annotations
    reg_annots: List[RegAnnotation] = []
    for reg, val in rsod.registers.items():
        if val == 0:
            continue
        idx = resolve_addr_to_image(val, images)
        if idx >= 0:
            offset = val - _img_base(idx)
            r = resolve_address(tc, images[idx].elf, f"0x{offset:x}", images[idx].map_file, images[idx].efi_path, images[idx].pe_base)
            reg_annots.append(RegAnnotation(reg=reg, addr=val, resolve=r))

    # Stack scan
    stack_scan: List[RegAnnotation] = []
    for qw in rsod.stack_qwords:
        if qw == 0:
            continue
        idx = resolve_addr_to_image(qw, images)
        if idx < 0:
            continue
        offset = qw - _img_base(idx)
        r = resolve_address(tc, images[idx].elf, f"0x{offset:x}", images[idx].map_file, images[idx].efi_path, images[idx].pe_base)
        if r.status != "unknown":
            stack_scan.append(RegAnnotation(reg="", addr=qw, resolve=r))

    # ── Apply source root remapping to all resolved paths ────

    source_roots = args.source_roots
    for sf in frames:
        if sf.resolve.file:
            sf.resolve.file = remap_source_path(sf.resolve.file, source_roots)
    for ra in reg_annots:
        if ra.resolve.file:
            ra.resolve.file = remap_source_path(ra.resolve.file, source_roots)

    # ── Resolve GitHub info for markdown ──────────────────────

    gh_repo = args.repo or ""
    gh_commit = args.commit or ""
    if args.markdown and not gh_repo:
        gh_repo, gh_commit = _detect_github_info()
    if args.repo and not gh_commit:
        _, gh_commit = _detect_github_info()

    # ── Output ────────────────────────────────────────────────

    if args.json:
        emit_json(arch, images, rsod, frames, reg_annots, stack_scan)
    elif args.markdown:
        emit_markdown(arch, images, rsod, frames, reg_annots, stack_scan,
                      tc, args.detail, gh_repo, gh_commit, source_roots)
    else:
        emit_human(arch, images, rsod, frames, reg_annots, stack_scan, tc, args.detail)


if __name__ == "__main__":
    main()
