#!/usr/bin/env python3
"""check-no-avx.py — assert no UNGATED VEX/EVEX instruction reaches a produced image.

AVX is perfectly usable under UEFI — AXL ships AVX2 kernels and runs them.
What is not usable is UNGATED AVX. Firmware enables SSE state (the calling
convention needs XMM) but leaves `CR4.OSXSAVE` clear, so an AVX instruction
traps with `#UD - Invalid Opcode` until a CPL0 caller turns the state on with
`axl_cpu_enable_avx()` — per logical processor, and only if the CPU has AVX at
all. This gate is about instructions that run BEFORE, or WITHOUT, that check.

Two separate wrong conclusions were reached in one session because the fault
reads like a pointer bug (`CR2 = 0` is the tell that it is NOT one —
disassemble the RIP instead).

The instruction does not have to be *ours*. Two ways it arrives:

  * a compile that lost `$(GCC_ARCH)` (`-march=x86-64`). This host's gcc
    defaults to `-march=x86-64-v3`, so a bare `g++ -O2` emits VEX for plain
    scalar `double` math.
  * an archive member built by someone else with a higher baseline. RHEL 10's
    `libstdc++.a` is the live example: `tree.o` is VEX-free (it does no FP), so
    `std::map` runs, while `hashtable_c++0x.o` carries 49 VEX instructions for
    the hash load-factor math, so `std::unordered_map` faults.

AXL does ship AVX on purpose. `axl_cpu_avx_usable()` reads `CR4.OSXSAVE` and
`XCR0` on the live core before dispatching, and `axl_cpu_enable_avx()` lets a
CPL0 UEFI app turn the state on. Code reached only through such a check is
correct, so this gate cannot ban the encoding outright — it bans the encoding
in any symbol NOT named in AVX_GATED_SYMBOLS below. Attribution is per SYMBOL,
not per object, so an object that already holds one dispatched AVX routine
still fails if a SECOND function in it acquires VEX by accident.

Detection is by ENCODING, not by mnemonic. In 64-bit mode the opcodes 0xC4,
0xC5 (`LES`/`LDS`) and 0x62 (`BOUND`) are all invalid as legacy instructions
and are reused as the 3-byte VEX, 2-byte VEX and EVEX prefixes respectively.
A VEX prefix may not be preceded by a legacy prefix, so the FIRST byte of an
instruction is decisive — but only of a real instruction. objdump wraps a long
encoding onto a continuation line carrying bytes and no mnemonic, and
`movabs $0x346dc5d63886594b,%rcx` in `axl-tcp-sync.o` wraps with `c5` leading
the continuation. Requiring the mnemonic field is what makes that a non-event;
the naive `grep -E '\\bv[a-z0-9]+\\s'` reports it as AVX.

A second, independent detector cross-checks the mnemonic column: any `v...`
mnemonic outside the small set of legacy `v` instructions must also have been
caught by the byte scan, and a disagreement FAILS the run rather than being
silently resolved — a gate whose two views disagree cannot be trusted in
either direction.

Prefer the `.so` over the `.efi`. Both carry the same code, but `objcopy` does
not carry `.symtab` into the PE image, and without symbols nothing can be
attributed to an allowlist entry. An unattributable hit is reported as a
failure, not waved through.

Non-x86 inputs are reported as skipped, not silently passed: AArch64 has no
VEX encoding, so there is nothing here to check.

Usage: check-no-avx.py <image-or-object> [...]
  Accepts anything objdump can disassemble: ELF .so, .o, .a, PE/COFF .efi.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# 0xC4 / 0xC5 / 0x62 lead byte == VEX3 / VEX2 / EVEX in 64-bit mode.
VEX_LEAD_BYTES = frozenset({"c4", "c5", "62"})

# Mnemonics that begin with `v` but are NOT VEX-encoded. Without this set the
# cross-check would flag ordinary segment/VMX/SVM instructions.
LEGACY_V_MNEMONICS = frozenset({
    "verr", "verw",
    "vmcall", "vmclear", "vmfunc", "vmlaunch", "vmload", "vmmcall",
    "vmptrld", "vmptrst", "vmread", "vmresume", "vmrun", "vmsave",
    "vmwrite", "vmxoff", "vmxon",
})

# VEX-encoded, but general-purpose-register only: BMI1/BMI2/TBM. These carry a
# VEX prefix and no `v` in the mnemonic, and — unlike everything else here —
# they touch no YMM state, so `CR4.OSXSAVE` is irrelevant to them and they run
# under UEFI as-is.
#
# They are still reported, because they are still above the `-march=x86-64`
# baseline the SDK targets and still `#UD` on a CPU that lacks BMI. But they
# are reported for THAT reason, and they go through the same symbol allowlist:
# routing them to the detector-disagreement branch instead (which is what an
# earlier revision did) made them unallowlistable and blamed OSXSAVE for a
# fault it has nothing to do with.
VEX_GPR_MNEMONICS = frozenset({
    "andn", "bextr", "blci", "blcic", "blcmsk", "blcs", "blsfill", "blsi",
    "blsic", "blsmsk", "blsr", "bzhi", "mulx", "pdep", "pext", "rorx",
    "sarx", "shlx", "shrx", "t1mskc", "tzmsk",
})

# Symbols allowed to contain AVX because they are reached ONLY through a
# runtime check of CR4.OSXSAVE + XCR0 — the state UEFI leaves clear. Adding an
# entry here is a claim that such a check guards every call site; make that
# claim only after reading the dispatch, and say where it lives.
AVX_GATED_SYMBOLS: dict[str, str] = {
    "blend_row_over_avx2":
        "src/gfx/axl-gfx.c — dispatched behind axl_cpu_avx_usable(), which "
        "reads CR4.OSXSAVE then XCR0 bit 2 on the live core",
    "LzFind_SaturSub_256":
        "deps/lzma/LzFind.c (vendored LZMA SDK) — dispatched behind "
        "CpuArch.c's CPUID + xgetbv probe",
}

# `  4f:\tc5 fb 5e c1          \tvdivsd %xmm1,%xmm0,%xmm0`
#   addr   ^ raw bytes           ^ mnemonic + operands
# A continuation line carrying the tail of a long encoding has no mnemonic
# field and so does not match — its bytes belong to the instruction above it.
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]{2}(?: [0-9a-f]{2})*)\s*\t(\S+)")

# `0000000000001234 <blend_row_over_avx2>:`
SYMBOL_RE = re.compile(r"^[0-9a-f]+ <([^>]+)>:")


def run_objdump(tool: str, args: list[str], path: Path) -> str | None:
    try:
        return subprocess.run(
            [tool, *args, str(path)],
            check=True, capture_output=True, text=True,
        ).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


def objdump_for(path: Path) -> str:
    """Pick the objdump that can read @a path.

    Host objdump reads x86-64 ELF and PE/COFF alike; only an AArch64 ELF
    object needs the cross tool.
    """
    if run_objdump("objdump", ["-f"], path) is not None:
        return "objdump"
    return "aarch64-linux-gnu-objdump"


def scan(path: Path) -> tuple[list[str], list[str]]:
    """Disassemble @a path and return (findings, notes)."""
    if not path.exists():
        return ([f"{path}: not found (build it first)"], [])

    tool = objdump_for(path)
    header = run_objdump(tool, ["-f"], path)
    if header is None:
        return ([f"{path}: {tool} could not read the file"], [])
    if "i386" not in header and "x86-64" not in header:
        return ([], [f"{path}: not x86 — no VEX encoding exists here, skipped"])

    disasm = run_objdump(tool, ["-d"], path)
    if disasm is None:
        return ([f"{path}: {tool} -d failed"], [])

    findings: list[str] = []
    notes: list[str] = []
    disagreements: list[str] = []
    gated_hits: dict[str, int] = {}
    symbol = ""
    for line in disasm.splitlines():
        sym = SYMBOL_RE.match(line)
        if sym is not None:
            symbol = sym.group(1)
            continue
        insn = INSN_RE.match(line)
        if insn is None:
            continue
        addr, raw, mnemonic = insn.group(1), insn.group(2), insn.group(3)
        lead = raw.split(" ")[0]
        # `(bad)` is objdump decoding alignment padding, not an instruction.
        by_bytes = lead in VEX_LEAD_BYTES and not mnemonic.startswith("(bad)")
        by_name = mnemonic.startswith("v") and mnemonic not in LEGACY_V_MNEMONICS

        # The BYTE scan is authoritative: it reads the actual encoding. The
        # mnemonic scan is a weaker heuristic kept only to catch a gap in the
        # byte scan, so it can only ever ADD a finding, never veto one.
        if by_bytes:
            if symbol in AVX_GATED_SYMBOLS:
                gated_hits[symbol] = gated_hits.get(symbol, 0) + 1
            elif not symbol:
                findings.append(
                    f"{path}:{addr}: {mnemonic} [{raw.strip()}] — no enclosing "
                    "symbol, so it cannot be matched against the gated "
                    "allowlist. Point this gate at the .so, not the stripped "
                    ".efi.")
            elif mnemonic in VEX_GPR_MNEMONICS:
                findings.append(
                    f"{path}: {symbol}+{addr}: {mnemonic} [{raw.strip()}] — "
                    "VEX-encoded GPR op: needs no XSAVE state, but is above "
                    "the -march=x86-64 baseline and #UDs without BMI")
            else:
                findings.append(
                    f"{path}: {symbol}+{addr}: {mnemonic} [{raw.strip()}]")
        elif by_name:
            disagreements.append(
                f"{path}:{addr}: {mnemonic} [{raw.strip()}] — VEX mnemonic, "
                "non-VEX lead byte")

    if disagreements:
        findings.append(
            f"{path}: DETECTOR DISAGREEMENT — the byte scan and the mnemonic "
            "scan reached different verdicts, so neither can be trusted until "
            "the gap is closed:")
        findings.extend(f"  {d}" for d in disagreements)

    # Say what was exempted. A gate that silently drops hits reads as "nothing
    # to report" when the truth is "reported nothing on purpose".
    for sym, count in sorted(gated_hits.items()):
        notes.append(f"{path}: {count} AVX instruction(s) in {sym} — allowed: "
                     f"{AVX_GATED_SYMBOLS[sym]}")
    return (findings, notes)


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]]
    if not paths:
        print("usage: check-no-avx.py <image-or-object> [...]", file=sys.stderr)
        return 2

    findings: list[str] = []
    notes: list[str] = []
    for path in paths:
        f, n = scan(path)
        findings.extend(f)
        notes.extend(n)

    # A missing input is a build problem, not an AVX finding. Reported under
    # its own banner so it cannot be misread as "an AVX instruction reached a
    # produced image" -- the scanned path is the .so, which make builds as a
    # side effect of the .efi rule and therefore cannot regenerate on its own.
    absent = [f for f in findings if f.endswith("(build it first)")]
    if absent:
        print("check-no-avx: FAIL — input(s) missing, so nothing was scanned:")
        for a in absent:
            print(f"  {a}")
        return 1

    if findings:
        print("check-no-avx: FAIL — an UNGATED VEX/EVEX instruction reached a "
              "produced image. UEFI boots with CR4.OSXSAVE clear and the SDK "
              "targets -march=x86-64, so this is #UD at runtime, not a slow "
              "path (each line below says which of the two applies):")
        for f in findings:
            print(f"  {f}")
        print("  Fix at the source: compile with -march=x86-64 (the SDK's "
              "$(GCC_ARCH)), stop linking the archive member that carries it, "
              "or -- if a CR4.OSXSAVE check really does guard every call site "
              "-- add the symbol to AVX_GATED_SYMBOLS with that justification.")
        return 1

    for n in notes:
        print(f"check-no-avx: {n}")
    print(f"check-no-avx: clean — no ungated VEX/EVEX encoding in "
          f"{len(paths)} image(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
