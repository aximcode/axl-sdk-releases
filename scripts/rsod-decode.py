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
            and follows gnu_debuglink to find the .debug ELF; failing
            that it reads SizeOfImage / ImageBase straight from the
            optional header and disassembles the PE itself
    .map    MSVC / lld-link linker map (also spelled --map) — function
            names and the preferred load address, no image required
    .pdb    MSVC program database (also spelled --pdb) — the ONLY route
            to source lines on MSVC; see below

Architecture is auto-detected from ELF/PE magic bytes (no external
`file` command needed — works on Windows).

THE PE + .map WORKFLOW (MSVC-built images)
------------------------------------------
An MSVC-built UEFI image ships as a PE with a `/MAP` beside it: no ELF,
no `.debug`, no PDB. Each artifact is independently sufficient, and each
enables a different half of the job:

    given       yields
    .map only   symbols, function + offset, preferred load address,
                a ONE-DIRECTIONAL wrong-map check (see below)
    image only  disassembly, image bounds, full image/dump validation
    both        everything, plus a map-vs-image bound

VALIDATION IS NOT EQUAL ACROSS THOSE ROWS, and the difference matters
because the failure mode is silence, not an error. Handed the wrong
build, this script produces coherent, confident, entirely fictional
symbols -- plausible names and a plausible call chain, because .text
often does not move between builds of the same source even when the
data does.

    image only  SizeOfImage against the dump's own record, in BOTH
                directions. Any difference is caught.
    .map only   a symbol cannot live beyond the end of its image, so a
                highest symbol offset at or past the size the dump
                records is a proven contradiction. This catches a map
                too BIG for the dump and CANNOT catch one that is too
                small. It is a real check, not the equal of the image
                one.
    both        the same symbol bound, measured against the image's OWN
                SizeOfImage. This is the only check that catches a stale
                .map sitting beside a CORRECT .efi -- where the image
                passes its own size check, the dump-side bound does not
                run because a PE supplied the size, and the map still
                wins symbol resolution, so every signal reports healthy
                while the symbols are fiction.

A map and an image also each carry a link stamp -- the map's
`Timestamp is` and the PE's TimeDateStamp are written from the same
value. A difference is reported, but as a NOTE rather than a mismatch,
because it is evidence and not proof: the PE's copy does not survive
every build flow. Every .efi in this tree carries TimeDateStamp 0,
written by the ELF-to-PE conversion, and a flow that rewrote it to some
other value would make an inequality accuse a correct pair. Zero is
ignored outright.

A map-only run prints its link stamp under `Symbol sources:` precisely
because the machine cannot close the gap: it is the fingerprint a human
can check against the build that produced the crash.

The load base need not be given. It is stated by the PE optional header,
by the map's "Preferred load address", and by the firmware's own
loaded-image list; where the module list disagrees with the header the
module list wins, because that means the image was relocated.

PDB AS A SYMBOL SOURCE
----------------------
On MSVC the PDB is the only route to source line numbers: `/MAPINFO:LINES`
is a fatal error on current linkers (`LNK1117`), so a linker map carries
function names and nothing finer.

A PE records an ABSOLUTE build-host path to its PDB plus a CodeView
GUID/age. Only the basename can be looked for on the machine doing the
decoding, and archived release artifacts are routinely renamed per
version — so the PDB sits beside the image, matched, and unused. Three
things follow:

    1. A PDB beside the image is found under the embedded basename, and
       failing that by matching the CodeView GUID/age, which survives
       renaming. No flag needed in either case.
    2. `--pdb FILE` names one outright, for a PDB kept elsewhere.
    3. A PDB whose GUID/age disagrees with the image is REFUSED and
       reported, not silently trusted. A mismatched PDB yields specific
       source lines, which are believed precisely because they are
       specific.

The report always names the symbol sources it used, and says why line
numbers are missing when they are — the three cases (no PDB, PDB present
but unmatched, PDB used) were previously distinguishable only by noticing
that no line number had appeared.

IMAGE / DUMP VALIDATION
-----------------------
Handed a wrong-but-plausible image — a different build of the same
source — a decoder with no cross-check emits specific, confident,
entirely fictional symbols. Two gates run before any symbol is printed,
and a failure is reported as a banner above the report (and as
`image_warnings` in --json):

    1. SizeOfImage from the PE header vs the size the dump's own
       loaded-image list records at that base. A mismatch is proof.
    2. The faulting PC and each branch record must decode to an
       instruction boundary, anchored on the containing function's
       start. Against the wrong image they land mid-instruction.

OUTPUT MODES
------------
Compact (default):
    Cause, fault address, crash location one-liner, stack trace with
    function/file:line, register code-pointer annotations.

Detailed (--detail):
    Everything in compact mode, plus: raw exception data, ESR decode,
    disassembly with interleaved source around fault instruction,
    image paths, bases and which source decided each base.

    The stack memory scan is NOT gated behind --detail when the firmware
    printed no frame list: in that case it is the best evidence in the
    dump, and it used to sit below a frame-pointer chain that had more
    prominence and less warrant.

    When a disassembly cannot be produced, the reason is printed. It
    used to print nothing, which reads as "there was nothing to show".

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
                   --rsod rsod.txt

AARCH64 RSODs include per-frame module names and bases, which the
script uses to match addresses to the correct image automatically.
Unresolved frames show "module+offset (no debug image)".

EXAMPLES
--------
Parse an RSOD from a serial log file. --rsod takes a WHOLE console
capture — the dump does not have to be extracted from it first:
    rsod-decode.py --image IpmiTool.efi --rsod rsod_log.txt

MSVC-built PE with a sibling linker map, base inferred:
    rsod-decode.py --image app.efi --detail --rsod putty-session.log

Symbols from the map alone, no image available:
    rsod-decode.py --map app.map --rsod console.log

Pipe RSOD text from clipboard:
    pbpaste | rsod-decode.py --image IpmiTool.debug

Detailed output with disassembly:
    rsod-decode.py --image app.debug --detail --rsod rsod.txt

Markdown report with GitHub links:
    rsod-decode.py --image app.debug --markdown --rsod rsod.txt > crash.md

Manual address decode:
    rsod-decode.py --image app.debug --base 0x6A3C0000 --addr 0x6A3C02EB

JSON output:
    rsod-decode.py --image app.debug --json --rsod rsod.txt

Remap build paths to local source:
    rsod-decode.py --image app.debug --source-root ~/projects/edk2 --rsod rsod.txt

(--file is still accepted everywhere --rsod is, so existing scripts and
muscle memory keep working.)

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
import atexit
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
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
    # Where the containing function starts (RVA), and how far into it the
    # address landed. -1 means the resolver could not say; 0 is a real answer
    # and a loud one -- it means the address IS the function entry.
    func_start: int = -1
    func_offset: int = -1


@dataclass
class Image:
    elf: str = ""
    base: Optional[int] = None
    name: str = ""
    size: int = 0x20000
    map_file: str = ""     # linker .map file (function-level resolution)
    efi_path: str = ""     # original PE path (disassembly, PE DWARF, bounds)
    pe_base: int = 0       # preferred load address from .map or PE header
    pe_size: int = 0       # SizeOfImage from the PE header (0 = not a PE)
    # Map-side identity. `map_max_rva` is the highest symbol offset the map
    # declares: a lower bound on the image it was linked from, and the only
    # size-like fact a map yields without guessing at section alignment.
    # `map_timestamp` is the link stamp the map header prints, identical to
    # the PE's TimeDateStamp for the same link.
    map_max_rva: int = 0
    map_timestamp: int = 0
    # A link-stamp difference between map and image: evidence, not proof, so
    # it is a note rather than a mismatch warning. Kept separate from
    # `sym_note`, which explains missing LINE NUMBERS and would be clobbered.
    stamp_note: str = ""
    base_source: str = ""  # how `base` was decided, for the --detail line
    # Two different claims, kept apart: `warnings` says the IMAGE cannot be the
    # one the dump came from (every symbol is then suspect); `pdb_warnings`
    # says only that the line numbers would have been wrong.
    warnings: list[str] = field(default_factory=lambda: list[str]())
    pdb_warnings: list[str] = field(default_factory=lambda: list[str]())
    pdb_file: str = ""     # PDB in use for line numbers
    pdb_source: str = ""   # named | beside the image | matched by GUID
    # The path handed to llvm-addr2line. Differs from efi_path only when the
    # PDB had to be staged under the basename the PE embeds (see stage_pdb).
    symbolize_path: str = ""
    # Why line numbers are unavailable, when they are. One line, printed.
    sym_note: str = ""


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
class BranchAnnotation:
    """A resolved last-branch record. The firmware hands these over for free
    and they beat every heuristic in the tool: the target names the function
    that was entered (at offset 0, that it was entered at all) and the source
    names the caller, with no stack walking involved."""
    index: int = 0
    from_addr: int = 0
    to_addr: int = 0
    from_resolve: ResolveResult = field(default_factory=ResolveResult)
    to_resolve: ResolveResult = field(default_factory=ResolveResult)


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
    # The firmware's own loaded-image list: [(base, size, name), ...]. This is
    # the only statement in the whole dump about where an image was ACTUALLY
    # loaded and how big it was, which makes it both the base of last resort
    # and the one thing that can prove the operator handed over the wrong file.
    loaded_images: list[tuple[int, int, str]] = field(
        default_factory=lambda: list[tuple[int, int, str]]())
    # Last-branch records: [(index, from_pc, to_pc), ...]. Exact, free, and
    # better than any heuristic — the target names the function that was
    # entered and the source names its caller.
    branch_records: list[tuple[int, int, int]] = field(
        default_factory=lambda: list[tuple[int, int, int]]())
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
    """Detect file type from magic bytes: 'elf', 'pe', 'pdb', 'map', or ''."""
    try:
        with open(path, "rb") as f:
            magic = f.read(32)
            if magic[:4] == b"\x7fELF":
                return "elf"
            if magic[:2] == b"MZ":
                return "pe"
            if magic == _MSF_MAGIC:
                return "pdb"
    except OSError:
        return ""
    return "map" if _looks_like_linker_map(path) else ""


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


_PE_MACHINE = {
    0xAA64: "AARCH64",   # IMAGE_FILE_MACHINE_ARM64
    0x8664: "X64",       # IMAGE_FILE_MACHINE_AMD64
    0x014C: "IA32",      # IMAGE_FILE_MACHINE_I386
}


@dataclass
class PeHeader:
    """The optional-header fields that decide where an image lives, plus the
    CodeView record naming its debug file.

    `size_of_image` is the whole loaded span, headers and .bss included, which
    is what the firmware's loaded-image list reports and what the image-bounds
    check needs. Deriving a size from the last symbol instead (what this script
    did for years) both undercounts -- it stops at the last NAMED thing -- and
    is unavailable for a PE whose only symbols are in a .map.

    `pdb_path` is the ABSOLUTE build-host path the linker recorded
    (`C:/build/obj/app.pdb`, backslashed in reality), which almost never
    exists on the machine doing
    the decoding -- only its basename can be looked for. `pdb_guid` / `pdb_age`
    are the identity that says whether a given PDB belongs to this image, and
    they survive renaming, which the basename does not.
    """
    machine: str = ""
    image_base: int = 0
    size_of_image: int = 0
    timestamp: int = 0     # COFF TimeDateStamp; a linker map states the same value
    pdb_path: str = ""
    pdb_guid: str = ""
    pdb_age: int = 0


def _read_pe_header(path: str) -> Optional[PeHeader]:
    """Parse a PE/COFF optional header. None if @path is not a PE."""
    try:
        with open(path, "rb") as f:
            mz = f.read(64)
            if len(mz) < 64 or mz[:2] != b"MZ":
                return None
            # PE header offset is at MZ+0x3C (little-endian DWORD)
            pe_offset = struct.unpack("<I", mz[0x3C:0x40])[0]
            f.seek(pe_offset)
            if f.read(4) != b"PE\x00\x00":
                return None
            machine = struct.unpack("<H", f.read(2))[0]
            # TimeDateStamp is COFF offset 4: Machine(2) NumberOfSections(2)
            # then the stamp. A linker map prints the SAME value in its
            # header, which is what lets a map be checked against an image.
            coff_rest = f.read(6)
            timestamp = (struct.unpack("<I", coff_rest[2:6])[0]
                         if len(coff_rest) == 6 else 0)

            # Skip the rest of the COFF header (18 bytes after Machine) to the
            # optional header; its Magic says which width ImageBase has.
            f.seek(pe_offset + 4 + 20)
            opt = f.read(64)
            if len(opt) < 64:
                return PeHeader(machine=_PE_MACHINE.get(machine, ""),
                                timestamp=timestamp)
            magic = struct.unpack("<H", opt[0:2])[0]
            if magic == 0x20B:        # PE32+
                image_base = struct.unpack("<Q", opt[24:32])[0]
            elif magic == 0x10B:      # PE32
                image_base = struct.unpack("<I", opt[28:32])[0]
            else:
                return PeHeader(machine=_PE_MACHINE.get(machine, ""),
                                timestamp=timestamp)
            # SizeOfImage sits at optional-header offset 56 in both widths.
            size_of_image = struct.unpack("<I", opt[56:60])[0]
            hdr = PeHeader(machine=_PE_MACHINE.get(machine, ""),
                           image_base=image_base,
                           size_of_image=size_of_image,
                           timestamp=timestamp)

            f.seek(0)
            _read_pe_codeview(f.read(), pe_offset, magic, hdr)
            return hdr
    except (OSError, struct.error):
        return None


def _guid_str(raw: bytes) -> str:
    """Format a 16-byte CodeView GUID the way every PDB tool prints it.

    The first three fields are little-endian and the last two are byte order
    as stored, which is the whole trick of this format and the reason a naive
    hex dump of the same bytes does not match `llvm-pdbutil`.
    """
    if len(raw) < 16:
        # A truncated record is exactly what this tool is pointed at, so it
        # reports one rather than raising through the middle of a decode.
        return ""
    d1, d2, d3 = struct.unpack("<IHH", raw[:8])
    tail = "".join(f"{b:02X}" for b in raw[10:16])
    return f"{{{d1:08X}-{d2:04X}-{d3:04X}-{raw[8]:02X}{raw[9]:02X}-{tail}}}"


def _read_pe_codeview(data: bytes, pe_offset: int, magic: int,
                      hdr: PeHeader) -> None:
    """Fill in @hdr's PDB path/GUID/age from the debug directory, if present."""
    # Data directory 6 is IMAGE_DIRECTORY_ENTRY_DEBUG. It sits after the
    # windows-specific fields, whose length is the only thing PE32 and PE32+
    # differ by here.
    ddir = pe_offset + 4 + 20 + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack("<II", data[ddir + 6 * 8:ddir + 6 * 8 + 8])
    if not rva or not size:
        return

    nsec = struct.unpack("<H", data[pe_offset + 6:pe_offset + 8])[0]
    opt_size = struct.unpack("<H", data[pe_offset + 20:pe_offset + 22])[0]
    sec = pe_offset + 4 + 20 + opt_size

    def to_offset(r: int) -> Optional[int]:
        for i in range(nsec):
            b = sec + i * 40
            vsize, vaddr, rsize, praw = struct.unpack("<IIII", data[b + 8:b + 24])
            if vaddr <= r < vaddr + max(vsize, rsize):
                return praw + (r - vaddr)
        return None

    off = to_offset(rva)
    if off is None:
        return
    for i in range(size // 28):
        e = off + i * 28
        dtype = struct.unpack("<I", data[e + 12:e + 16])[0]
        dsize = struct.unpack("<I", data[e + 16:e + 20])[0]
        praw = struct.unpack("<I", data[e + 24:e + 28])[0]
        if dtype != 2:              # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        cv = data[praw:praw + dsize]
        if cv[:4] != b"RSDS":       # NB10 (PDB 2.0) carries no GUID
            continue
        hdr.pdb_guid = _guid_str(cv[4:20])
        hdr.pdb_age = struct.unpack("<I", cv[20:24])[0]
        hdr.pdb_path = cv[24:].split(b"\x00")[0].decode("utf-8", "replace")
        return


_MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"


def _read_pdb_identity(path: str) -> Optional[Tuple[str, int]]:
    """(GUID, age) from a PDB's own Info stream, or None if unreadable.

    Read directly rather than via `llvm-pdbutil dump --summary`: it is a
    superblock, a block-map indirection and one 28-byte record, and doing it
    here means a mismatched PDB is still caught on a machine that has
    llvm-addr2line but not the rest of the LLVM tools.
    """
    # Seek rather than slurp. A production PDB is 100 MB to 1 GB, the GUID
    # scan opens every .pdb in the directory in turn, and the answer is 28
    # bytes behind a superblock and two block indirections.
    try:
        with open(path, "rb") as f:
            head = f.read(56)
            if head[:32] != _MSF_MAGIC:
                return None
            block_size, _fpm, _nblocks, dir_bytes, _unk, block_map = \
                struct.unpack("<IIIIII", head[32:56])
            if block_size == 0 or dir_bytes == 0:
                return None

            def block(n: int) -> bytes:
                f.seek(n * block_size)
                return f.read(block_size)

            nblk = (dir_bytes + block_size - 1) // block_size
            idx = struct.unpack_from(f"<{nblk}I", block(block_map), 0)
            raw = b"".join(block(b) for b in idx)[:dir_bytes]

            nstreams = struct.unpack_from("<I", raw, 0)[0]
            sizes = struct.unpack_from(f"<{nstreams}I", raw, 4)
            pos = 4 + nstreams * 4
            stream1: Tuple[int, ...] = ()
            for i, sz in enumerate(sizes):
                n = 0 if sz == 0xFFFFFFFF else (sz + block_size - 1) // block_size
                if i == 1:
                    stream1 = struct.unpack_from(f"<{n}I", raw, pos)
                    break
                pos += n * 4
            if not stream1:
                return None
            info = block(stream1[0])            # the header is in block 0
            if len(info) < 28:
                return None
            _ver, _sig, age = struct.unpack_from("<III", info, 0)
            guid = _guid_str(info[12:28])
            return (guid, age) if guid else None
    except (OSError, struct.error, IndexError):
        return None


def _detect_pe_machine(path: str) -> str:
    """Read PE Machine field to determine architecture."""
    hdr = _read_pe_header(path)
    return hdr.machine if hdr else ""


def _looks_like_linker_map(path: str) -> bool:
    """True when @path is an MSVC/lld `/MAP` listing.

    A map is a first-class symbol source, not a stray text file: it carries
    every public symbol AND the preferred load address. Recognizing one by
    content rather than by extension means `--image foo.map` works, which is
    what a user reaches for when the map is all they have.
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            head = f.read(64 * 1024)
    except OSError:
        return False
    if re.search(r"Preferred load address is\s+[0-9a-fA-F]+", head):
        return True
    # lld-link and older MSVC maps without the preamble: the Publics-by-Value
    # table is the structure that makes a map a map.
    return bool(re.search(r"^\s+\w+:[0-9a-fA-F]{8}\s+\S+\s+[0-9a-fA-F]{16}",
                          head, re.M))


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
    elif btype == "map":
        return ""          # symbols come from the map itself
    elif btype == "pdb":
        print(f"Error: a PDB resolves lines only alongside its image: "
              f"pass --image <file>.efi --pdb {path}", file=sys.stderr)
        sys.exit(1)
    else:
        print(f"Error: unrecognized file type: {path}", file=sys.stderr)
        sys.exit(1)


# One temp dir for every staged PDB, removed at exit.
_stage_dir = ""


def _stage_pdb(img: Image, pdb_path: str, embedded_name: str) -> str:
    """Symlink the image and its PDB into a temp dir, PDB under @embedded_name.

    llvm-addr2line has no flag naming a PDB: it looks for the absolute path the
    PE recorded, then that path's BASENAME beside the image. So the way to
    point it at `app-1.2.3.efi.pdb` is to present that file under the name the
    PE asks for. Symlinks in a scratch dir do it without touching the
    directory the user gave us -- which may be a read-only archive, and is
    never ours to write into.
    """
    global _stage_dir
    if not _stage_dir:
        _stage_dir = tempfile.mkdtemp(prefix="rsod-pdb-")
        atexit.register(shutil.rmtree, _stage_dir, True)

    sub = os.path.join(_stage_dir, f"img{len(os.listdir(_stage_dir))}")
    os.makedirs(sub, exist_ok=True)
    staged_img = os.path.join(sub, os.path.basename(img.efi_path))
    try:
        os.symlink(os.path.abspath(img.efi_path), staged_img)
        os.symlink(os.path.abspath(pdb_path),
                   os.path.join(sub, embedded_name))
    except OSError:
        return ""
    return staged_img


def _resolve_pdb(img: Image, hdr: Optional[PeHeader], named_pdb: str,
                 quiet: bool) -> None:
    """Decide which PDB (if any) provides line numbers, and record why.

    On MSVC the PDB is the ONLY route to source lines -- `/MAPINFO:LINES` is a
    fatal error on current linkers, so a map cannot carry them. Before this,
    the PDB could only be DISCOVERED, by the basename the PE embeds, and the
    failure was silent: archived release artifacts are renamed per version, so
    the PDB routinely sat beside the image, matched, and unused.
    """
    if not img.efi_path or hdr is None:
        return
    embedded = os.path.basename(hdr.pdb_path.replace("\\", "/")) if hdr.pdb_path else ""
    want = (hdr.pdb_guid, hdr.pdb_age) if hdr.pdb_guid else None
    directory = os.path.dirname(os.path.abspath(img.efi_path))

    candidate, source = "", ""
    if named_pdb:
        candidate, source = named_pdb, "named"
    elif embedded and os.path.isfile(os.path.join(directory, embedded)):
        candidate, source = os.path.join(directory, embedded), "beside the image"
    elif want is not None:
        # The renaming case. Ask every PDB in the directory who it belongs to;
        # the identity survives a rename, the filename does not.
        for entry in sorted(os.listdir(directory) if os.path.isdir(directory) else []):
            if not entry.lower().endswith(".pdb"):
                continue
            path = os.path.join(directory, entry)
            if _read_pdb_identity(path) == want:
                candidate, source = path, "matched by GUID"
                break

    if not candidate:
        # An ELF beside the image already supplies file:line, so a missing PDB
        # costs nothing and saying "no line numbers" next to a report full of
        # them is worse than silence.
        if img.elf:
            return
        if embedded:
            img.sym_note = (f"No PDB: image embeds '{embedded}', not found "
                            f"beside the image - no line numbers")
        elif hdr.machine:
            img.sym_note = "No PDB: the image names none - no line numbers"
        return

    # Verify the pairing. An unreadable identity on either side is NOT a
    # refutation -- it means the question could not be asked, and refusing a
    # PDB on that basis would break every PDB this reader cannot parse.
    got = _read_pdb_identity(candidate) if want is not None else None
    if want is not None and got is not None and got != want:
        img.pdb_warnings.append(
            f"{os.path.basename(candidate)} is {got[0]} age {got[1]}, but the "
            f"image was built against {hdr.pdb_guid} age {hdr.pdb_age}")
        img.sym_note = (f"No PDB: {os.path.basename(candidate)} belongs to a "
                        f"different build - no line numbers")
        return

    # llvm-addr2line finds a PDB only under the name the PE recorded, so
    # anything else has to be presented under that name.
    if embedded and os.path.abspath(candidate) != os.path.join(directory, embedded):
        staged = _stage_pdb(img, candidate, embedded)
        if not staged:
            # Symlinks can be unavailable (Windows without developer mode).
            # Saying "PDB, matched by GUID" here would assert that a file was
            # used which was never handed to the symbolizer.
            img.sym_note = (f"No PDB: cannot present "
                            f"{os.path.basename(candidate)} as '{embedded}' "
                            f"for the symbolizer - no line numbers")
            return
        img.symbolize_path = staged
    elif not embedded:
        # Nothing to stage under, and llvm-addr2line looks for the recorded
        # name only, so a named PDB could not be reached even though it exists.
        img.sym_note = (f"No PDB: the image records no PDB name, so "
                        f"{os.path.basename(candidate)} cannot be located by "
                        f"the symbolizer - no line numbers")
        return

    if not _which("llvm-addr2line"):
        img.sym_note = ("No PDB: llvm-addr2line not on PATH, so "
                        f"{os.path.basename(candidate)} cannot be read - "
                        "no line numbers")
        return

    img.pdb_file = candidate
    img.pdb_source = source
    if not quiet:
        print(f"  Found PDB: {os.path.basename(candidate)} ({source})",
              file=sys.stderr)


def register_image(spec: str, tc: Toolchain, quiet: bool = False,
                   named_pdb: str = "") -> Image:
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

    btype = _detect_binary_type(os.path.realpath(file_path))
    elf = resolve_image_file(file_path, quiet=quiet)
    name = re.sub(r"\.(debug|dll|efi|so|map|pdb)$", "",
                  os.path.basename(file_path))

    # A .map or .pdb given directly IS the symbol source; there is no image
    # beside it. Everything downstream keys off which artifact we actually
    # hold, so record that here rather than inferring it from empty strings.
    efi_path = "" if btype in ("map", "pdb") else file_path
    map_file = file_path if btype == "map" else ""
    if btype == "pdb" and not named_pdb:
        named_pdb = file_path

    # Look for a .map file alongside the image. The candidate has to be a
    # DIFFERENT file that actually reads as a linker map: `re.sub` on an
    # extensionless image (`--image elfprobe`) substitutes nothing, so the
    # image matched itself and was registered as its own map -- announced as
    # "Found map file: elfprobe", which is nearly convincing.
    if not map_file:
        for map_candidate in [
            re.sub(r"\.(efi|dll|debug|so)$", ".map", file_path),  # foo.map
            file_path + ".map",                                     # foo.efi.map
        ]:
            if (map_candidate != file_path and os.path.isfile(map_candidate)
                    and _looks_like_linker_map(map_candidate)):
                map_file = map_candidate
                if not quiet:
                    print(f"  Found map file: {map_file}", file=sys.stderr)
                break

    # SizeOfImage is authoritative, free, and needs no symbols. Read it before
    # falling back to the last-symbol estimate, which undercounts by whatever
    # trails the final named symbol (.bss, in practice a lot) and left every
    # address past the hard-coded 128 KB guess looking like it belonged to no
    # image at all.
    pe_hdr = _read_pe_header(file_path) if btype == "pe" else None
    pe_size = pe_hdr.size_of_image if pe_hdr else 0

    # Preferred load address: the map states it outright, and so does the PE
    # optional header. Both are the LINK-time base -- addresses in the map and
    # the VMAs objdump prints are relative to it, so it is what turns a runtime
    # offset back into something those two tools understand.
    pe_base = _map_preferred_base(map_file) if map_file else 0
    if not pe_base and pe_hdr:
        pe_base = pe_hdr.image_base

    # Estimate size from last symbol
    size = 0x20000
    if pe_size:
        size = pe_size
    elif elf:
        nm_out = _run([tc.nm, "-C", "-n", elf])
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1].lower() in ("t", "d", "b"):
                try:
                    size = int(parts[0], 16) + 0x1000
                except ValueError:
                    pass
    elif map_file:
        # Estimate size from map file addresses. These are `Rva+Base`, i.e.
        # ABSOLUTE -- subtract the preferred base or a map-only image comes out
        # sized like the whole address space.
        #
        # A map that states no preferred load address (lld-link's plain form,
        # which _looks_like_linker_map deliberately accepts) leaves pe_base at
        # 0, and subtracting nothing is exactly the failure the paragraph above
        # claims to prevent: a 5 GB image that then contains every value in the
        # dump, so SP and the flags register get annotated as code. Only trust
        # the arithmetic when the result is a plausible image size.
        est = 0
        for rva, _, _ in _map_entries(map_file):
            end = rva - pe_base + 0x1000
            if end > est:
                est = end
        if 0 < est <= 256 * 1024 * 1024:
            size = max(size, est)

    # Map-side identity, recorded whether or not a PE is also present. The
    # size estimate above runs only when there is NO PE, so reusing it would
    # have left the map unchecked in exactly the case where both artifacts are
    # available and the strongest check is possible.
    map_max_rva = 0
    map_timestamp = 0
    if map_file:
        map_ents = _map_entries(map_file)
        if map_ents:
            map_max_rva = map_ents[-1][0] - pe_base   # entries are sorted
        map_timestamp = _map_timestamp(map_file)

    img = Image(elf=elf, base=base, name=name, size=size,
                map_file=map_file, efi_path=efi_path, pe_base=pe_base,
                pe_size=pe_size,
                map_max_rva=map_max_rva, map_timestamp=map_timestamp,
                base_source="--image :BASE" if base is not None else "")
    _resolve_pdb(img, pe_hdr, named_pdb, quiet)
    return img


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
        # Alignment here as well as on the seed. A frame pointer that cannot
        # BE one is not a frame pointer at any depth, and a stack that is
        # partially overwritten -- the usual reason to be reading one of these
        # at all -- routinely holds a plausible-looking misaligned qword one
        # link in. Guarding only the entry point stopped the fabricated frame
        # the report complained about and left the same fabrication reachable
        # one link deeper, where nothing else rejects it on AArch64
        # (monotonic is False there).
        if cur == 0 or cur & 7 or cur < base or cur + 16 > end:
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
    # A frame pointer that cannot BE one is not a starting point. The guard was
    # `fp == 0` alone, so a dump whose BP was 0x452AF2D9 -- odd, and therefore
    # impossible as a frame pointer on either architecture -- was walked
    # anyway, and the single fabricated frame it produced was printed above the
    # stack scan that had the real answer. Emit nothing rather than a fake
    # frame: the caller falls back to the seeded fault PC, and the scan leads.
    if fp == 0 or fp & 7:
        return []
    # No range check here on purpose: _walk_fp_chain already refuses to read
    # outside the captured window, and it does so AFTER seeding frame 0 from
    # LR. Rejecting the whole walk up front would throw away AArch64's link
    # register -- a genuine return address -- whenever the stack dump did not
    # happen to span the frame pointer.
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
# Load base inference + image/dump validation
# ═══════════════════════════════════════════════════════════════

def _dump_image_for(img: Image, rsod: RsodData) -> Optional[tuple[int, int, str]]:
    """The firmware's loaded-image entry for @img, or None.

    Matched by SizeOfImage first and by name second, and the order is the whole
    point: files get renamed on the way to the person decoding them (the
    capture that motivated this arrived as `app.efi` while the firmware called
    it `psa.efi`), but SizeOfImage is a property of the build. A size match is
    proof of identity; a name match is a reasonable guess.
    """
    if img.pe_size:
        hits = [e for e in rsod.loaded_images if e[1] == img.pe_size]
        if len(hits) == 1:
            return hits[0]
    want = img.name.lower()
    for entry in rsod.loaded_images:
        if re.sub(r"\.efi$", "", entry[2], flags=re.I).lower() == want:
            return entry
    return None


def infer_image_base(img: Image, rsod: RsodData, first: bool,
                     default_base: Optional[int]) -> None:
    """Decide where @img was loaded, recording WHICH source said so.

    A UEFI image loaded at its preferred base is the common case, and that base
    is stated independently by the PE optional header, the map's `Preferred
    load address` and the firmware's own module list. Making `:BASE` mandatory
    when all three agree is the tool declining to read its own inputs.

    Runtime evidence outranks link-time evidence throughout, because the one
    case where they disagree is the one that matters: the image was relocated.
    """
    if img.base is not None:
        return                                  # explicit :BASE wins outright

    if img.name in rsod.module_bases:
        img.base = rsod.module_bases[img.name]
        img.base_source = "dump stack frames"
        return
    if first and rsod.parsed_base is not None:
        img.base = rsod.parsed_base
        img.base_source = "dump ImageBase"
        return
    if default_base is not None:
        img.base = default_base
        img.base_source = "--base"
        return

    entry = _dump_image_for(img, rsod)
    if entry:
        img.base = entry[0]
        img.base_source = "dump loaded-image list"
        return
    if img.pe_base:
        img.base = img.pe_base
        img.base_source = ("map preferred load address" if img.map_file
                           else "PE ImageBase")


def _instruction_addresses(tc: Toolchain, img: Image, start_va: int,
                           stop_va: int) -> Optional[set[int]]:
    """Instruction start addresses in [start_va, stop_va), or None if unknown.

    None and the empty set are different answers and must stay that way: None
    means the question could not be ASKED (no binary, no objdump, no anchor to
    decode from), and reporting that as "no instruction there" would turn a
    missing tool into an accusation that the operator brought the wrong file.
    """
    # ELF first, matching image_va() / func_start_va() / disasm_lines(). The
    # reverse order here meant that for an image with BOTH artifacts -- a .efi
    # that resolved to a sibling .debug, the ordinary EDK2 pairing -- the
    # addresses were computed in ELF space and handed to objdump running on the
    # PE, so the instruction-boundary gate could never fire.
    binary = img.elf or img.efi_path
    if not binary or not _which(tc.objdump):
        return None
    out = _run([tc.objdump, "-d", "--no-show-raw-insn",
                f"--start-address=0x{start_va:x}",
                f"--stop-address=0x{stop_va:x}", binary])
    if not out:
        return None
    addrs = {int(m.group(1), 16)
             for m in re.finditer(r"^\s+([0-9a-f]+):\t", out, re.M)}
    return addrs or None


def image_va(img: Image, rva: int) -> int:
    """The address objdump and nm use for @rva in the binary we will decode.

    A PE's disassembly is addressed against its LINK base (`ImageBase`, or the
    map's preferred load address), NOT against wherever the firmware loaded it
    -- so a relocated image has to be mapped back before it can be decoded. An
    ELF .debug/.so is already addressed the way the offset is.

    Which one applies is decided by `img.elf`, NOT by `pe_base` or `efi_path`:
    an ELF with a sibling .map -- exactly what an EDK2 build leaves behind --
    has BOTH a non-zero `pe_base` (from the map's preferred load address) and a
    non-empty `efi_path`, and adding the two decodes empty space. gdb handles
    that case itself, so the damage only surfaces on the binutils fallback.
    """
    if img.elf or not img.efi_path:
        return rva
    return img.pe_base + rva if img.pe_base else rva


def func_start_va(tc: Toolchain, img: Image, rva: int) -> Optional[int]:
    """Start of the function containing @rva, in image_va() space.

    nm for an ELF, the linker map for a PE. Both the disassembler and the
    instruction-boundary check need this and need it to agree: decoding from
    the function entry is what makes a listing line up with the real
    instruction stream instead of resynchronizing partway through it.
    """
    if img.elf:
        best = 0
        for a, _name in _get_nm_symbols(tc.nm, img.elf, ""):
            if a <= rva and a > best:
                best = a
            if a > rva:
                break
        return best or None
    if img.map_file:
        base = _map_preferred_base(img.map_file)
        hit = _map_lookup(img.map_file, base + rva)
        if hit:
            return image_va(img, hit[0] - base)
    return None


def _decodes_at_boundary(tc: Toolchain, img: Image, addr: int) -> Optional[bool]:
    """Does @addr land on an instruction boundary in @img? None = can't tell.

    Anchored on the containing function's start, so the decode is the one the
    CPU would have done. x86 disassembly resynchronizes, so starting anywhere
    would usually converge and usually agree -- but "usually" is not a basis
    for telling someone their symbols are fiction.
    """
    if img.base is None:
        return None
    rva = addr - img.base
    if not 0 <= rva < img.size:
        return None

    file_va = image_va(img, rva)
    anchor = func_start_va(tc, img, rva)
    if anchor is None or not 0 <= file_va - anchor <= 0x4000:
        # No anchor, or one too far away to decode cheaply and honestly.
        return None

    addrs = _instruction_addresses(tc, img, anchor, file_va + 16)
    if addrs is None:
        return None
    return file_va in addrs


def validate_image(tc: Toolchain, img: Image, rsod: RsodData) -> None:
    """Fill img.warnings when the image cannot be the one the dump came from.

    A decoder that cannot tell it was handed the wrong binary is worse than one
    that declines to answer, because the reader has no reason to doubt it.
    Handed a different build of the same source -- same project, same version,
    different configuration -- this script produced specific, confident,
    entirely fictional symbols with no signal anywhere in the output.

    Both checks are proof rather than heuristic, and both are nearly free.
    """
    if img.base is None:
        return

    # Gate 1: SizeOfImage against the firmware's own record for that base.
    #
    # Gate 1b covers the case Gate 1 cannot see. `pe_size` is 0 when the
    # symbols came from a .map with no PE beside it, so for years --map
    # skipped this check entirely and answered a wrong map with confident,
    # fully-formatted fiction while --image refused the same build. A map has
    # no SizeOfImage, but it does carry a fact the dump can contradict:
    #
    #     a symbol cannot live beyond the end of its own image.
    #
    # So a highest symbol offset at or past the size the firmware recorded is
    # a hard contradiction, not a heuristic -- reported with the same
    # confidence as the PE mismatch, worded to blame the map rather than an
    # image that was never supplied.
    #
    # KNOWN LIMIT, and it must not be oversold: this is ONE-DIRECTIONAL. It
    # catches a map too BIG for the dump. A wrong map that happens to be
    # smaller still resolves silently, so map-only remains weaker than
    # --image, which compares an exact size in both directions.
    for base, size, _name in rsod.loaded_images:
        if base != img.base:
            continue
        if img.pe_size and size != img.pe_size:
            img.warnings.append(
                f"SizeOfImage is 0x{img.pe_size:x}, but the dump's "
                f"loaded-image list records 0x{size:x} at base 0x{base:x}")
        elif not img.pe_size and img.map_max_rva and img.map_max_rva >= size:
            img.warnings.append(
                f"the map's highest symbol is at +0x{img.map_max_rva:x}, past "
                f"the end of the 0x{size:x} image the dump records at base "
                f"0x{base:x} -- this map is not for this dump "
                f"(a map that is too SMALL cannot be caught this way)")
        break

    # Gate 1c: a map and an image supplied together. Same invariant as 1b --
    # a symbol cannot live beyond the end of its own image -- measured
    # against the image's OWN SizeOfImage rather than the dump's record, so
    # it holds even when the dump has no entry for this base.
    #
    # This is the case NEITHER other gate can see: a stale .map beside a
    # CORRECT .efi. Gate 1 passes because the image really is the dump's, and
    # 1b does not run because a PE supplied the size -- yet the map wins
    # symbol resolution, so every symbol is fiction while all other signals
    # report healthy.
    if img.map_max_rva and img.pe_size and img.map_max_rva >= img.pe_size:
        img.warnings.append(
            f"the map's highest symbol is at +0x{img.map_max_rva:x}, past the "
            f"end of this 0x{img.pe_size:x} image -- the map and the image are "
            f"not from the same build")

    # A link-stamp difference is EVIDENCE, not proof, and is reported as a
    # note rather than a mismatch. The map's `Timestamp is` and the PE's COFF
    # TimeDateStamp are written from the same value by the linker -- but the
    # PE's copy does not survive every build flow. This very tree's images all
    # carry TimeDateStamp 0, because the ELF-to-PE conversion writes no stamp;
    # a flow that rewrites it to some other non-zero value would make an
    # inequality here accuse a perfectly good pair. Zero is skipped outright.
    if img.map_timestamp and img.efi_path:
        pe_hdr = _read_pe_header(img.efi_path)
        if pe_hdr and pe_hdr.timestamp and pe_hdr.timestamp != img.map_timestamp:
            img.stamp_note = (
                f"map linked 0x{img.map_timestamp:x}, image stamped "
                f"0x{pe_hdr.timestamp:x} -- different builds, or the image's "
                f"stamp was rewritten after linking")

    # Gate 2: the recorded addresses must decode to instruction boundaries.
    # Against the wrong image all three landed mid-instruction.
    checks: list[tuple[str, int]] = []
    if rsod.fault_pc:
        try:
            checks.append(("faulting PC", int(rsod.fault_pc, 16)))
        except ValueError:
            pass
    for idx, frm, to in rsod.branch_records:
        if frm:
            checks.append((f"branch source LBRfr{idx}", frm))
        if to:
            checks.append((f"branch target LBRto{idx}", to))

    for label, addr in checks:
        if _decodes_at_boundary(tc, img, addr) is False:
            img.warnings.append(
                f"{label} 0x{addr:x} does not land on an instruction boundary")


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


# Parsed map files, keyed by path. A real map is well over a megabyte and was
# re-read and re-parsed once per ADDRESS -- roughly thirty times for a dump
# with a register annotation set and a stack scan.
_map_cache: Dict[str, list[tuple[int, str, str]]] = {}
_map_base_cache: Dict[str, int] = {}
_map_ts_cache: Dict[str, int] = {}


def _map_entries(map_file: str) -> list[tuple[int, str, str]]:
    """Symbols from a linker map as sorted (absolute address, name, object).

    Parses the MSVC / lld-link `/MAP` Publics-by-Value table:
      0001:00000070  main  0000000180001070  hello.o
      0001:0002fae4  ?fInit@@YAHXZ  000000018002fe24  f   PciLib.obj
    The optional `f` flags the symbol as a function and is NOT the object file;
    reading it as one put the letter "f" in the report's source column.
    """
    if map_file in _map_cache:
        return _map_cache[map_file]

    entries: list[tuple[int, str, str]] = []
    try:
        with open(map_file, encoding="utf-8", errors="replace") as f:
            for line in f:
                # The flag column may hold more than `f`: MSVC also emits `i`
                # for an import thunk and `f i` together. Anchoring on `f`
                # alone dropped those entries entirely, and since _map_lookup
                # returns the nearest symbol at or below, every address inside
                # a dropped thunk was then attributed to the preceding
                # function -- a confident wrong name.
                m = re.match(
                    r"\s+\w+:\w+\s+(\S+)\s+(0x[0-9a-fA-F]+|[0-9a-fA-F]{16})"
                    r"\s+(?:[a-z]\s+)*(\S+)\s*$",
                    line)
                if m:
                    try:
                        entries.append((int(m.group(2), 16), m.group(1),
                                        m.group(3)))
                    except ValueError:
                        continue
    except OSError:
        return []

    # Drop symbols below the image: MSVC emits absolute symbols
    # (`__AbsoluteZero`, `___safe_se_handler_count`) at address 0, which are
    # not code, sort ahead of everything, and would otherwise be the "nearest
    # symbol at or below" answer for any address the base does not cover -- and
    # print as a negative RVA in --dump.
    base = _map_preferred_base(map_file)
    entries = [e for e in entries if e[0] >= base]

    entries.sort(key=lambda e: e[0])
    _map_cache[map_file] = entries
    return entries


def _map_timestamp(map_file: str) -> int:
    """The map's own `Timestamp is <hex>` link stamp, or 0 if it states none.

    MSVC and lld-link both print this, and it is the SAME value the PE stores
    in the COFF TimeDateStamp. The RSOD dump never records a timestamp, so
    this cannot be checked against a dump -- but it identifies a link exactly,
    which makes it the one hard check available when a map and an image are
    supplied together, and a fingerprint a human can verify by hand when only
    a map is.
    """
    if map_file in _map_ts_cache:
        return _map_ts_cache[map_file]
    ts = 0
    try:
        with open(map_file, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"Timestamp is\s+([0-9a-fA-F]+)", line)
                if m:
                    ts = int(m.group(1), 16)
                    break
                # The stamp sits in the first few header lines; stop before
                # walking a 20 MB symbol table looking for one that is absent.
                if re.match(r"\s+\w+:\w+\s+\S+\s+[0-9a-fA-F]{16}", line):
                    break
    except OSError:
        pass
    _map_ts_cache[map_file] = ts
    return ts


def _map_preferred_base(map_file: str) -> int:
    """The map's own `Preferred load address`, or 0 if it states none."""
    if map_file in _map_base_cache:
        return _map_base_cache[map_file]
    base = 0
    try:
        with open(map_file, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"Preferred load address is\s+([0-9a-fA-F]+)",
                              line)
                if m:
                    base = int(m.group(1), 16)
                    break
    except OSError:
        pass
    _map_base_cache[map_file] = base
    return base


def _map_lookup(map_file: str, addr: int) -> Optional[tuple[int, str, str]]:
    """Nearest map symbol at or below @addr (absolute), or None."""
    entries = _map_entries(map_file)
    best: Optional[tuple[int, str, str]] = None
    for entry in entries:
        if entry[0] <= addr:
            best = entry
        else:
            break
    return best


def _resolve_address_map(map_file: str, offset_hex: str) -> ResolveResult:
    """Resolve an offset using a linker .map file (function-level only)."""
    result = ResolveResult()
    if not map_file or not os.path.isfile(map_file):
        return result

    try:
        offset = int(offset_hex, 16)
    except ValueError:
        return result

    if not _map_entries(map_file):
        return result

    preferred_base = _map_preferred_base(map_file)

    # The map file has absolute addresses (preferred_base + RVA).
    # The offset from RSOD resolution is relative to image base.
    # Try: offset + preferred_base (most common), then offset as-is.
    for addr in [offset + preferred_base, offset]:
        hit = _map_lookup(map_file, addr)
        if hit:
            sym_addr, name, obj = hit
            result.func = name
            result.file = obj
            result.status = "map_symbol"
            # How far INTO the function the fault landed. The map has the
            # function start, so this is arithmetic the tool already had the
            # inputs for -- and it is the number that makes a crash findable in
            # source, which a repeated RVA is not.
            result.func_start = sym_addr - preferred_base
            result.func_offset = addr - sym_addr
            return result

    return result


def resolve_address(tc: Toolchain, elf: str, offset_hex: str,
                    map_file: str = "", pe_base: int = 0,
                    symbolize: str = "") -> ResolveResult:
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

    # Try llvm-addr2line on the PE -- for embedded DWARF, and for the PDB it
    # locates beside the image. `symbolize` is the staged copy when the PDB had
    # to be presented under the name the PE recorded.
    if symbolize and symbolize.endswith(".efi"):
        result = _resolve_address_pe_dwarf(symbolize, offset_hex, pe_base)
        if result.status != "unknown":
            return result

    # Fall back to .map file (function-level only, no line numbers)
    if map_file:
        return _resolve_address_map(map_file, offset_hex)

    return ResolveResult()


def _resolve_in_image(tc: Toolchain, img: Image, offset: int) -> ResolveResult:
    """resolve_address() for an offset into a registered Image.

    Every caller was spelling out the same fields of the same Image, which is
    how `pe_base` came to be threaded through some call sites and not others.
    """
    return resolve_address(tc, img.elf, f"0x{offset:x}", img.map_file,
                           img.pe_base,
                           img.symbolize_path or img.efi_path)


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

    # Format-independent: printed alongside the dump proper by the firmwares
    # that print them at all, and none belongs to one parser.
    _parse_loaded_images(text, data)
    _parse_branch_records(text, data)
    _parse_axl_crash_report(text, data)

    return data


def _section_lines(text: str, heading: str) -> list[str]:
    """Lines of a `Heading:` block, up to the first blank line.

    Scoped rather than matched globally on purpose: a crash report's stack
    frames and its loaded-image rows are both `  0x<hex> ...`, and telling them
    apart by shape alone is the kind of regex that works until an image is
    named `???`.
    """
    out: list[str] = []
    in_section = False
    for line in text.splitlines():
        stripped = line.strip()
        if not in_section:
            if stripped == heading:
                in_section = True
            continue
        if not stripped:
            break
        out.append(line)
    return out


def _parse_axl_crash_report(text: str, data: RsodData):
    """Parse AXL's own CrashHandler report (drivers/crashhandler/report.c).

    Every other format here is somebody else's; this one is ours, and it was
    the one the decoder read WORST. The registers and the faulting PC came
    through (they look like the Dell `REG=VALUE` shape), and everything that
    makes the report worth writing did not: the `Image:` line stating the load
    base, the `Loaded Images:` table, and every frame of the `Stack Trace:`
    section. The report's own last line tells the reader to run this script.
    """
    # "Image:        crashtest (base 0x6A3C0000, size 0xA000)" -- the faulting
    # image, and the same statement EDK2 spells as `ImageBase=`.
    m = re.search(r"^Image:\s+(\S+)\s+\(base\s+0x([0-9a-fA-F]+),"
                  r"\s*size\s+0x([0-9a-fA-F]+)\)", text, re.M)
    if m:
        name, base, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        data.module_bases.setdefault(name, base)
        if data.parsed_base is None:
            data.parsed_base = base
        if not any(e[0] == base for e in data.loaded_images):
            data.loaded_images.append((base, size, name))

    # "Exception:    #UD (Invalid Opcode) at 0x18000027A". The diagnosis engine
    # speaks EDK2's `0e(#PF)` spelling, so normalize into that rather than
    # duplicating the vector table -- otherwise our own reports come out with
    # no exception type and no cause at all, which is most of the summary
    # block.
    m = re.search(r"^Exception:\s+(\S+)(?:\s+\(([^)]*)\))?\s+at\s+0x",
                  text, re.M)
    if m and not data.exception_type:
        mnemonic, desc = m.group(1), (m.group(2) or "")
        vec = next((v for v, info in _X64_EXCEPTIONS.items()
                    if info[0] == mnemonic), None)
        if vec is not None:
            data.exception_type = f"{vec:02x}({mnemonic})"
        else:
            data.exception_type = f"{mnemonic} ({desc})" if desc else mnemonic

    # "  0x000000006A3C0000 0x00A000  crashtest" under "Loaded Images:".
    for line in _section_lines(text, "Loaded Images:"):
        m = re.match(r"\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$",
                     line)
        if not m:
            continue          # the "Base Size Name" header row
        base, size, name = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if not base:
            continue
        data.module_bases.setdefault(name, base)
        if not any(e[0] == base for e in data.loaded_images):
            data.loaded_images.append((base, size, name))

    # "  0x000000006A3C02EB  crashtest+0x2EB", or "  0x... ???" for an address
    # the handler could not attribute.
    frames: list[tuple[int, str, int, int]] = []
    for line in _section_lines(text, "Stack Trace:"):
        m = re.match(r"\s+0x([0-9a-fA-F]+)\s+(?:(\S+)\+0x([0-9a-fA-F]+)|\?\?\?)",
                     line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        if m.group(2) is None:
            frames.append((pc, "", 0, 0))
            continue
        name, off = m.group(2), int(m.group(3), 16)
        frames.append((pc, name, pc - off, off))
        data.module_bases.setdefault(name, pc - off)

    if data.exception_type and not data.cause:
        data.cause = (_diagnose_x64(data) if data.arch == "X64"
                      else _diagnose_aarch64(data))

    if not frames:
        return

    # A REAL frame list, so it replaces the single seeded fault PC the register
    # parser leaves behind -- otherwise the FP-chain fallback runs instead of
    # rendering the frames the firmware already walked. Frame 0 must be the
    # crash site; the handler's unwinder may or may not start there.
    fault = int(data.fault_pc, 16) if data.fault_pc else 0
    if fault and frames[0][0] != fault:
        frames.insert(0, (fault, "", 0, 0))
    data.stack_pcs = [f[0] for f in frames]
    data.stack_frame_info = [f for f in frames if f[1]]


def _parse_loaded_images(text: str, data: RsodData):
    """Parse the firmware's loaded-image list: `<base> <size> <name>.efi`.

    The size field is 8 hex digits and the name ends in `.efi`; a stack-dump
    row's second column is a 16-digit qword followed by an ASCII gutter, so the
    two cannot be confused. Both are anchored, because a serial capture is full
    of hex that means something else.
    """
    for m in re.finditer(
            r"^\s*([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8})\s+(\S+\.efi)(?:\s|$)",
            text, re.M):
        try:
            base = int(m.group(1), 16)
            size = int(m.group(2), 16)
        except ValueError:
            continue
        # Deduplicate. A reboot loop writes the report every boot, so one
        # console capture routinely holds the same image list several times --
        # and _dump_image_for accepts a SizeOfImage match only when exactly ONE
        # entry has that size, so the repeats made it reject its own answer and
        # fall back to the link-time base.
        entry = (base, size, m.group(3))
        if base and size and entry not in data.loaded_images:
            data.loaded_images.append(entry)


def _parse_branch_records(text: str, data: RsodData):
    """Parse last-branch records: `LBRfrN <addr> ...` / `LBRtoN <addr> ...`."""
    frm: Dict[int, int] = {}
    to: Dict[int, int] = {}
    for m in re.finditer(r"^\s*LBR(fr|to)(\d+)\s+([0-9a-fA-F]+)", text, re.M):
        try:
            idx, addr = int(m.group(2)), int(m.group(3), 16)
        except ValueError:
            continue
        (frm if m.group(1) == "fr" else to)[idx] = addr

    for idx in sorted(set(frm) | set(to)):
        data.branch_records.append((idx, frm.get(idx, 0), to.get(idx, 0)))


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
    # `NAME =VALUE` as well as `NAME=VALUE`: report.c pads short register names
    # to keep its columns square (`R8 =`, and `X0 =`..`X9 =`, `FP =` on aa64),
    # and a regex that required no space silently dropped every one of them.
    # Bounded to three spaces so this stays a register assignment and does not
    # start matching prose.
    for m in re.finditer(
            r"([A-Za-z][A-Za-z0-9_]*) {0,3}=(?:0x)?([0-9a-fA-F]{2,16})(?:\s|$|,)",
            text):
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
    branch_annots: Optional[List[BranchAnnotation]] = None,
    detail: bool = False
) -> None:
    title = images[0].name if images else "RSOD"
    print(bold(f"RSOD Decoder — {title} ({arch})"))
    if detail:
        for i, img in enumerate(images):
            base_str = f"  base=0x{img.base:x}" if img.base is not None else ""
            src = f" ({img.base_source})" if img.base_source else ""
            print(dim(f"Image {i}: {img.name}  "
                      f"{img.elf or img.efi_path or img.map_file}{base_str}{src}"))
    print()

    _emit_image_warnings(images)

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
                _emit_disasm(tc, images[sf.image_idx], sf.offset)
                if not tc.use_gdb:
                    r = sf.resolve
                    if r.file and r.line:
                        _emit_source(r.file, r.line)

    _emit_branches(branch_annots or [])

    # Deduplicate: skip register annotations that point to addresses already in the stack trace
    stack_addrs: set[int] = {sf.addr for sf in frames} if frames else set()
    unique_reg_annots = [ra for ra in reg_annots if ra.addr not in stack_addrs]

    if unique_reg_annots:
        print(f"\n{bold('Registers with code pointers:')}\n")
        for ra in unique_reg_annots:
            r = ra.resolve
            label = _sym_label(r)
            if r.file and r.line:
                print(f"  {bold(f'{ra.reg:<4}')} {green(f'{label:<30}')} {_clickable_loc(r.file, r.line)} {dim(f'[0x{ra.addr:x}]')}")
            else:
                print(f"  {bold(f'{ra.reg:<4}')} {cyan(f'{label:<30}')} {dim(f'[0x{ra.addr:x}]')}")

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

    # The stack scan used to be --detail-only and printed last, below a
    # frame-pointer chain labelled "Stack trace" that had, in the capture that
    # prompted this, invented its one frame from an odd BP. When the firmware
    # printed no frame list the scan is the best evidence in the dump -- it
    # reconstructed a complete entry-point-to-fault chain -- so it is no longer
    # gated behind a flag in exactly the case where nothing else survives.
    if stack_scan and (detail or not rsod.stack_frame_info):
        print(f"\n{bold('Stack scan (return addresses found in the stack dump):')}\n")
        for ra in stack_scan:
            r = ra.resolve
            label = _sym_label(r)
            if r.file and r.line:
                print(f"  {cyan(f'{label:<30}')} {_clickable_loc(r.file, r.line)} {dim(f'[0x{ra.addr:x}] (stack scan)')}")
            else:
                print(f"  {cyan(f'{label:<30}')} {dim(f'[0x{ra.addr:x}] (stack scan)')}")


def _sym_label(r: ResolveResult) -> str:
    """`Func + 0xA` — how far into the function the address landed.

    That number is what makes a crash findable in source or in a disassembly.
    The report used to print the RVA a second time instead, which tells the
    reader nothing they did not already have on the same line. -1 means the
    resolver could not say; 0 is a real answer (the function entry) and the
    resolvers that DO know say so, because "entered and faulted immediately"
    is a different bug from "faulted somewhere inside".
    """
    if r.func_offset >= 0:
        return f"{r.func} + 0x{r.func_offset:x}"
    return r.func


def _symbol_sources(img: Image) -> list[str]:
    """The artifacts actually consulted for this image, in report order."""
    out: list[str] = []
    if img.elf:
        out.append(f"{os.path.basename(img.elf)} (ELF/DWARF)")
    if img.map_file:
        # The link stamp, when the map states one. A map-only decode has no
        # other build fingerprint a human can check by hand, and the size
        # bound in validate_image only catches a map that is too BIG -- so
        # printing this is what lets someone confirm the artifact themselves
        # in the cases the machine cannot.
        stamp = (f", linked 0x{img.map_timestamp:x}"
                 if img.map_timestamp else "")
        out.append(f"{os.path.basename(img.map_file)} (map{stamp})")
    if img.pdb_file:
        out.append(f"{os.path.basename(img.pdb_file)} (PDB, {img.pdb_source})")
    return out


def _emit_image_warnings(images: List[Image]):
    """Print image/dump mismatches before anything that depends on them."""
    multi = len(images) > 1

    # A PDB from the wrong build is its own failure, and a worse one than a
    # wrong image: it yields specific SOURCE LINES, which are believed exactly
    # because they are so specific. Reported separately so the reader is not
    # told the whole decode is fiction when only the lines would have been.
    def _banner(heading: str, pick) -> None:
        hit = [img for img in images if pick(img)]
        if not hit:
            return
        print(red(bold(heading)))
        for img in hit:
            tag = f"[{img.name}] " if multi else ""
            for w in pick(img):
                print(red(f"   - {tag}{w}"))
        print()

    _banner("!! PDB DOES NOT MATCH THE IMAGE - ignoring it for line numbers:",
            lambda i: i.pdb_warnings)
    _banner("!! IMAGE DOES NOT MATCH THE DUMP - symbols below are probably "
            "fiction:", lambda i: i.warnings)

    # Which artifacts answered, and -- the line worth printing -- why there
    # are no line numbers when there are none. The three PDB cases (absent,
    # present-but-renamed, present-and-used) were indistinguishable in the
    # output except by noticing that a line number had not appeared.
    for img in images:
        tag = f" [{img.name}]" if multi else ""
        sources = _symbol_sources(img)
        if sources:
            print(dim(f"Symbol sources{tag}: " + ", ".join(sources)))
        if img.stamp_note:
            print(dim(f"  note{tag}: {img.stamp_note}"))
        if img.sym_note:
            print(yellow(f"{img.sym_note}"))
    if any(_symbol_sources(i) or i.sym_note for i in images):
        print()


def _emit_branches(branch_annots: List[BranchAnnotation]):
    """Print resolved last-branch records.

    Parsed for years and never resolved: they were used only to detect the dump
    format and to OCR-correct their own hex digits. In the capture that
    prompted this they answered the question outright -- the target was the
    faulting function's ENTRY, so the fault happened on its first call, in the
    prologue, and the source named the caller with no stack walking at all.
    """
    if not branch_annots:
        return
    print(f"\n{bold('Branch records (last branch taken, most recent first):')}\n")
    for b in branch_annots:
        if b.from_addr:
            print(f"  from  {cyan(f'{_sym_label(b.from_resolve):<40}')} "
                  f"{dim(f'[0x{b.from_addr:x}]')}")
        if b.to_addr:
            entry = "  (function entry)" if b.to_resolve.func_offset == 0 else ""
            print(f"  to    {green(f'{_sym_label(b.to_resolve):<40}')} "
                  f"{dim(f'[0x{b.to_addr:x}]')}{entry}")


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

    label = _sym_label(r)
    if r.status == "resolved":
        if r.file and r.line:
            print(f"  {prefix} {green(f'{label:<30}')} {_clickable_loc(r.file, r.line)} {addr_info}{img_tag}")
        else:
            print(f"  {prefix} {green(f'{label:<30}')} {addr_info}{img_tag}")
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
        print(f"  {prefix} {cyan(f'{label:<30}')} {addr_info}{img_tag}")
    else:
        unknown = "???"
        print(f"  {prefix} {yellow(f'{unknown:<30}')} {addr_info}")


def disasm_lines(tc: Toolchain, img: Image,
                 offset: int) -> Tuple[List[str], str]:
    """Disassembly around @offset as (lines, reason-it-is-unavailable).

    Exactly one of the two is ever non-empty. Returning the REASON rather than
    an empty list is the point: `--detail` used to print nothing at all on this
    path, and nothing reads as "there was nothing to show".

    This used to be driven exclusively by `Image.elf`, which is "" for a PE
    with no sibling .debug -- so on the MSVC workflow nm and objdump both ran
    against an EMPTY PATH, returned nothing, and the function printed nothing
    at all. No disassembly, no warning, no hint that `--detail` had not done
    the thing its help text promises. The faulting instruction was the answer
    the whole time and objdump reads the PE perfectly well.

    So: fall back to the PE, take the function start from the .map when there
    is no nm to ask, and if genuinely nothing can be decoded, SAY SO.
    """
    # gdb interleaves source with disassembly, so prefer it when there is an
    # ELF for it to read -- but do not let its silence end the attempt, which
    # is what a bare `return` here used to do.
    if img.elf:
        gdb = _get_gdb_backend(tc, img.elf)
        if gdb:
            lines = gdb.disassemble(offset)
            if lines:
                return lines, ""

    binary = img.elf or img.efi_path
    if not binary:
        return [], "no image file for this module -- pass --image"
    if not _which(tc.objdump):
        return [], f"{tc.objdump} not on PATH"

    file_va = image_va(img, offset)
    func_start = func_start_va(tc, img, offset)

    # A pathological anchor (a huge function, or a bad map) would otherwise
    # dump thousands of lines above the one line the reader wants.
    if func_start is None or not 0 <= file_va - func_start <= 0x400:
        func_start = file_va

    # Decode PAST the trailing context wanted, then cut on an instruction
    # boundary. Stopping exactly at fault+24 lands objdump inside an
    # instruction, and it renders the remainder as `rex.W` / `.byte 0x89` --
    # which reads like the disassembler lost its place at the crash site.
    trailing = 4
    out = _run([
        tc.objdump, "-d", "-C", "--no-show-raw-insn",
        f"--start-address=0x{func_start:x}", f"--stop-address=0x{file_va + 96:x}",
        binary
    ])
    if not out:
        return [], f"{os.path.basename(binary)} produced no disassembly"

    fault_hex = f"{file_va:x}"
    lines: list[str] = []
    fault_idx = -1
    for line in out.splitlines():
        m = re.match(r"^\s+([0-9a-f]+):", line)
        if m:
            if m.group(1) == fault_hex:
                fault_idx = len(lines)
                lines.append(f"  {C_RED}>>>{C_NC} {line}")
            else:
                lines.append(f"      {line}")

    if fault_idx >= 0:
        lines = lines[:fault_idx + 1 + trailing]
    if not lines:
        return [], f"nothing decoded at 0x{file_va:x}"
    return lines, ""


def _emit_disasm(tc: Toolchain, img: Image, offset: int):
    """Print the disassembly block, or why there is none."""
    lines, reason = disasm_lines(tc, img, offset)
    if lines:
        print(f"\n{C_BOLD}Disassembly:{C_NC}\n")
        print("\n".join(lines))
    else:
        print(f"\n{C_BOLD}Disassembly:{C_NC} unavailable ({reason})")


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
    stack_scan: List[RegAnnotation],
    branch_annots: Optional[List[BranchAnnotation]] = None
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
            "function_offset": (f"0x{sf.resolve.func_offset:x}"
                                if sf.resolve.func_offset >= 0 else None),
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
            {"name": img.name,
             "base": f"0x{img.base:x}" if img.base is not None else None,
             "base_source": img.base_source or None,
             "size": f"0x{img.size:x}",
             "size_of_image": f"0x{img.pe_size:x}" if img.pe_size else None,
             "debug_elf": img.elf,
             "map_file": img.map_file or None,
             "pdb_file": img.pdb_file or None,
             "pdb_source": img.pdb_source or None,
             "symbol_sources": _symbol_sources(img),
             "no_line_info_reason": img.sym_note or None,
             "warnings": img.warnings,
             "pdb_warnings": img.pdb_warnings}
            for img in images
        ],
        # Hoisted out of "images" as well: a consumer that reads only the stack
        # trace must not be able to miss the one field saying the trace is
        # fiction. The human report prints this as a banner above everything.
        "pdb_warnings": [
            f"[{img.name}] {w}" if len(images) > 1 else w
            for img in images for w in img.pdb_warnings
        ],
        "image_warnings": [
            f"[{img.name}] {w}" if len(images) > 1 else w
            for img in images for w in img.warnings
        ],
        "stack_trace": [_frame_dict(sf) for sf in frames],
        "registers": {k: f"0x{v:x}" for k, v in rsod.registers.items()},
        "backtrace_recovered_via_fp": rsod.recovered_via_fp,
        "register_annotations": [_annot_dict(ra) for ra in reg_annots],
        "stack_scan": [_annot_dict(ra) for ra in stack_scan],
        "branch_records": [
            {"index": b.index,
             "from": f"0x{b.from_addr:x}" if b.from_addr else None,
             "from_function": b.from_resolve.func if b.from_addr else None,
             "from_offset": (f"0x{b.from_resolve.func_offset:x}"
                             if b.from_resolve.func_offset >= 0 else None),
             "to": f"0x{b.to_addr:x}" if b.to_addr else None,
             "to_function": b.to_resolve.func if b.to_addr else None,
             "to_offset": (f"0x{b.to_resolve.func_offset:x}"
                           if b.to_resolve.func_offset >= 0 else None),
             "to_is_function_entry": b.to_resolve.func_offset == 0}
            for b in (branch_annots or [])
        ],
    }
    print(json.dumps(data, indent=2))


def emit_markdown(
    arch: str, images: List[Image], rsod: RsodData,
    frames: List[StackFrame], reg_annots: List[RegAnnotation],
    _stack_scan: List[RegAnnotation], tc: Toolchain,
    detail: bool, repo: str, commit: str, source_roots: List[str],
    branch_annots: Optional[List[BranchAnnotation]] = None
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
            func = f"`{_sym_label(r)}`" if r.func != "???" else "???"
            loc = _md_link(r.file, r.line) if r.file and r.line else ""

        cols = [f"#{sf.frame}{note_str}", func, loc, off_str, f"`0x{sf.addr:x}`"]
        if multi:
            cols.append(img_name)
        return "| " + " | ".join(cols) + " |"

    # Header
    now = datetime.now(timezone.utc).astimezone()
    print(f"# RSOD Report — {images[0].name} ({arch})")
    print(f"\n*Generated: {now.strftime('%Y-%m-%d %H:%M:%S %Z')}*\n")

    # A markdown report gets pasted into a ticket and read by someone who was
    # not at the terminal, which makes it the LAST place a wrong-image warning
    # may be dropped.
    # Both verdicts, not just the image one. A refused PDB is the finer
    # failure -- it would have produced specific SOURCE LINES -- and a
    # markdown report is the copy that gets pasted into a ticket and read by
    # someone who was not at the terminal.
    if any(img.pdb_warnings for img in images):
        print("> **PDB DOES NOT MATCH THE IMAGE — line numbers were not "
              "taken from it:**")
        for img in images:
            tag = f"[{img.name}] " if multi else ""
            for w in img.pdb_warnings:
                print(f"> - {tag}{w}")
        print()
    if any(img.warnings for img in images):
        print("> **IMAGE DOES NOT MATCH THE DUMP — symbols below are probably "
              "fiction:**")
        for img in images:
            tag = f"[{img.name}] " if multi else ""
            for w in img.warnings:
                print(f"> - {tag}{w}")
        print()

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
        lines, reason = disasm_lines(tc, images[frames[0].image_idx],
                                     frames[0].offset)
        print("## Disassembly\n")
        if lines:
            print("```asm")
            for line in lines:
                print(re.sub(r"\033\[[0-9;]*m", "", line))
            print("```\n")
        else:
            print(f"*Unavailable: {reason}.*\n")

    # Branch records
    if branch_annots:
        print("## Branch Records\n")
        print("| # | Direction | Function | Address |")
        print("|---|-----------|----------|---------|")
        for b in branch_annots:
            if b.from_addr:
                print(f"| {b.index} | from | `{_sym_label(b.from_resolve)}` "
                      f"| `0x{b.from_addr:x}` |")
            if b.to_addr:
                entry = " *(function entry)*" if b.to_resolve.func_offset == 0 else ""
                print(f"| {b.index} | to | `{_sym_label(b.to_resolve)}`{entry} "
                      f"| `0x{b.to_addr:x}` |")
        print()

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
            func = f"`{_sym_label(r)}`" if r.func != "???" else "???"
            print(f"| {ra.reg} | {func} | {loc} | `0x{ra.addr:x}` |")
        print()

    # Images
    if detail:
        print("## Images\n")
        for img in images:
            base_str = f"`0x{img.base:x}`" if img.base is not None else "N/A"
            src = f", from {img.base_source}" if img.base_source else ""
            artifact = img.elf or img.efi_path or img.map_file
            print(f"- **{img.name}**: {artifact} (base: {base_str}{src})")
            sources = _symbol_sources(img)
            if sources:
                print(f"  - symbols: {', '.join(sources)}")
            if img.sym_note:
                print(f"  - *{img.sym_note}*")
        print()


def emit_dump(images: List[Image], tc: Toolchain):
    """Print every symbol the image's symbol source knows about.

    Same trap as the disassembler: driving nm with `img.elf` dumps NOTHING for
    a PE whose symbols live in a .map, and prints an empty section rather than
    saying why.
    """
    for img in images:
        if img.elf:
            print(f"Symbols: {img.elf} ({img.name})\n")
            out = _run([tc.nm, "-C", "-n", img.elf])
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1].lower() == "t":
                    print(f"  0x{parts[0]}  {parts[2]}")
        elif img.map_file:
            print(f"Symbols: {img.map_file} ({img.name})\n")
            base = _map_preferred_base(img.map_file)
            for addr, name, _obj in _map_entries(img.map_file):
                print(f"  0x{addr - base:016x}  {name}")
        else:
            print(f"Symbols: none available for {img.name} "
                  f"(no debug ELF and no .map)\n")
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
  # PE with a sibling linker map - no ELF, no PDB
  # (the MSVC workflow; the load base is inferred from the PE header,
  #  the map's preferred load address and the dump's own image list)
  rsod-decode.py --image app.efi --rsod console.log

  # Same, with the faulting instruction disassembled
  rsod-decode.py --image app.efi --detail --rsod console.log

  # Symbols from the map alone (no image available)
  rsod-decode.py --map app.map --rsod console.log

  # MSVC line numbers come from the PDB -- a map cannot carry them.
  # A PDB beside the image is found on its own, even if it has been
  # renamed (matched on the CodeView GUID, not the filename).
  # Name one explicitly when it lives somewhere else:
  rsod-decode.py --image app.efi --pdb archive/app-1.2.3.efi.pdb --rsod console.log

  # Raw terminal capture: the dump is embedded in unrelated console output
  # (login noise, a register block, a stack dump, a loaded-image list).
  # --rsod takes the WHOLE capture; it does not need trimming first.
  rsod-decode.py --image app.efi --rsod putty-session.log

  # Image not loaded at its preferred base
  rsod-decode.py --image app.efi:0x7E120000 --rsod console.log

  # ELF with DWARF, detailed output with disassembly and source context
  rsod-decode.py --image IpmiTool.debug --detail --rsod rsod.txt

  # Multi-image (AARCH64 cross-module stack trace)
  rsod-decode.py --image App.debug --image DxeCore.debug:0x47683000 --rsod rsod.txt

  # Markdown report with GitHub links
  rsod-decode.py --image App.debug --markdown --rsod rsod.txt > crash.md
  rsod-decode.py --image App.debug --markdown --repo org/repo --rsod rsod.txt

  # Remap build-machine paths to local source
  rsod-decode.py --image App.debug --source-root ~/projects/edk2 --rsod rsod.txt

  # Manual address decode
  rsod-decode.py --image App.debug --base 0x6A3C0000 --addr 0x6A3C02EB

  # JSON output / pipe from clipboard
  rsod-decode.py --image App.debug --json --rsod rsod.txt
  cat serial.log | rsod-decode.py --image App.debug

  # Dump all symbols
  rsod-decode.py --image App.debug --dump

For full documentation, run: python3 rsod-decode.py; pydoc3 rsod-decode
""",
    )
    parser.add_argument("--image", "--debug", action="append", dest="images", metavar="FILE[:BASE]",
                        help="EFI image (.debug, .dll, .so, .efi), a linker .map, "
                             "or a .pdb. Repeatable. Optional :BASE suffix "
                             "(inferred if omitted).")
    parser.add_argument("--map", action="append", dest="images", metavar="FILE[:BASE]",
                        help="Linker .map file — symbols and the preferred load "
                             "address, with no image needed. Same list as --image.")
    parser.add_argument("--pdb", metavar="FILE",
                        help="PDB providing line numbers. Overrides discovery; "
                             "needed when the PDB has been renamed, since the "
                             "image records only its build-time name.")
    parser.add_argument("--base", help="Default image base address (hex)")
    parser.add_argument("--arch", choices=["X64", "AARCH64", "IA32"], help="Override architecture")
    parser.add_argument("--rsod", "--file", dest="rsod_file", metavar="FILE",
                        help="RSOD text: an extracted dump, or a whole console capture "
                             "with the dump somewhere inside it")
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

    if args.pdb and not args.images:
        # A PDB is NOT self-sufficient the way a map is: it carries no section
        # table, and llvm-symbolizer refuses it as an object outright ("not
        # recognized as a valid object file"). It resolves lines only in
        # company with the image it was built from, so ask for that rather
        # than accepting the run and producing an empty report.
        print("Error: --pdb needs the image it belongs to; add "
              "--image <file>.efi", file=sys.stderr)
        sys.exit(1)
    if not args.images and not args.ocr and not args.rsod_file and not args.addr:
        parser.error("--image, --map, --pdb, --ocr, --rsod, or --addr is required")

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
    elif not args.addr and not args.dump and not sys.stdin.isatty():
        # --dump lists an image's symbols and reads no crash text, so waiting on
        # stdin for some is a hang, not a fallback: run from any script (stdin
        # is not a tty there) `--map foo.map --dump` blocked forever with no
        # output. Found by a test that hung on exactly that.
        rsod_text = sys.stdin.read()

    if rsod_text:
        rsod = parse_rsod(rsod_text)

    # Resolve architecture
    arch = args.arch or rsod.arch or elf_arch
    if not arch and args.images:
        # A linker map states no architecture, and a map-only run needs none:
        # nothing shells out to a prefixed nm/objdump for it. Refusing here
        # would make `--map foo.map --dump` impossible, which is precisely the
        # case where the map is all anyone has. The value only picks a tool
        # prefix, so it is inert on this path.
        specs = [re.sub(r":0x[0-9a-fA-F]+$", "", spec, flags=re.IGNORECASE)
                 for spec in args.images]
        if all(os.path.isfile(f) and _detect_binary_type(os.path.realpath(f)) == "map"
               for f in specs):
            arch = "X64"
    if not arch:
        print("Error: cannot detect arch, use --arch", file=sys.stderr)
        sys.exit(1)

    tc = Toolchain(arch)
    tc.check()

    # Register images
    images: List[Image] = []
    image_specs: list[str] = args.images or []
    for i, spec in enumerate(image_specs):
        # --pdb names the PDB for the FIRST image; a multi-image decode with
        # several renamed PDBs is not a case anyone has, and silently applying
        # one PDB to every image would be worse than not offering it.
        images.append(register_image(spec, tc, quiet=args.json,
                                     named_pdb=(args.pdb or "") if i == 0 else ""))

    # Apply bases: :BASE → dump frames → dump ImageBase → --base → the dump's
    # loaded-image list → the image's own preferred base.
    default_base = int(args.base, 16) if args.base else None
    for i, img in enumerate(images):
        infer_image_base(img, rsod, i == 0, default_base)

    # Now that every image has a base, check each one actually belongs to this
    # dump before a single symbol is printed from it.
    for img in images:
        validate_image(tc, img, rsod)

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
            sf.resolve = _resolve_in_image(tc, images[idx], sf.offset)
        elif addr in rsod_frame_map:
            mod_name, base, offset = rsod_frame_map[addr]
            sf.module_name = mod_name
            sf.offset = offset
        elif len(images) == 1 and images[0].base is not None:
            off = addr - _img_base(0)
            if 0 <= off < images[0].size:
                sf.image_idx = 0
                sf.offset = off
                sf.resolve = _resolve_in_image(tc, images[0], sf.offset)
        frames.append(sf)

    # Register annotations
    reg_annots: List[RegAnnotation] = []
    for reg, val in rsod.registers.items():
        if val == 0:
            continue
        idx = resolve_addr_to_image(val, images)
        if idx >= 0:
            offset = val - _img_base(idx)
            r = _resolve_in_image(tc, images[idx], offset)
            reg_annots.append(RegAnnotation(reg=reg, addr=val, resolve=r))

    # Stack scan
    stack_scan: List[RegAnnotation] = []
    for qw in rsod.stack_qwords:
        if qw == 0:
            continue
        idx = resolve_addr_to_image(qw, images)
        if idx < 0:
            continue
        r = _resolve_in_image(tc, images[idx], qw - _img_base(idx))
        if r.status != "unknown":
            stack_scan.append(RegAnnotation(reg="", addr=qw, resolve=r))

    # Branch records — resolve both ends against whichever image holds them.
    branch_annots: List[BranchAnnotation] = []
    for bidx, frm, to in rsod.branch_records:
        b = BranchAnnotation(index=bidx, from_addr=frm, to_addr=to)
        for addr, attr in ((frm, "from_resolve"), (to, "to_resolve")):
            if not addr:
                continue
            idx = resolve_addr_to_image(addr, images)
            if idx >= 0:
                setattr(b, attr,
                        _resolve_in_image(tc, images[idx], addr - _img_base(idx)))
        if b.from_addr or b.to_addr:
            branch_annots.append(b)

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
        emit_json(arch, images, rsod, frames, reg_annots, stack_scan,
                  branch_annots)
    elif args.markdown:
        emit_markdown(arch, images, rsod, frames, reg_annots, stack_scan,
                      tc, args.detail, gh_repo, gh_commit, source_roots,
                      branch_annots)
    else:
        emit_human(arch, images, rsod, frames, reg_annots, stack_scan, tc,
                   branch_annots, args.detail)


if __name__ == "__main__":
    main()
