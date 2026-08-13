#!/usr/bin/env python3
"""check-dogfood.py — dogfooding gates over AXL library code.

Two independent axes, both gating (either can fail CI):

  1. UEFI-call axis (a per-file ratchet, BASELINE below): library code should
     route UEFI protocol / boot-service calls through `axl_efi_call` / a backend
     function so every touchpoint stays enumerable and swappable.
  2. Allocation axis (marker-based, POOL_RE / POOL_MARKER below): library code
     dogfoods `axl_malloc` / `axl_free`; a RAW firmware `AllocatePool` /
     `FreePool` / `AllocatePages` / `FreePages` is allowed only with an inline
     `axl-pool-direct: <reason>` marker justifying the firmware-owned exception.

The rest of this doc covers the UEFI-call axis.
--- UEFI-call axis ---
flag raw UEFI protocol / boot-service calls in library code.

AXL's design rule (CLAUDE.md, "Backend Abstraction"): *all library code makes
UEFI protocol and boot-service calls through the backend functions and the
`axl_efi_call` macro*, never by calling a protocol method pointer directly. That
macro is a pass-through today (`#define axl_efi_call(fn, n, ...) (fn)(__VA_ARGS__)`),
so the payoff is not correctness — it is that **every UEFI touchpoint stays
enumerable and swappable**: a single seam you can hang tracing, error injection,
or an alternate backend on. A raw `gBS->RaiseTPL(...)` or `proto->Method(...)`
call is invisible to that seam, and each one is a small drift away from the
abstraction.

This gate does not demand a rewrite. It is a **ratchet**: it records the known
raw-call sites per file in `BASELINE` and fails only when a file *exceeds* its
number (a new violation) or a file with no debt gains one. Burn the debt down by
routing a call through `axl_efi_call` / a backend function and lowering the
number — the script prints the new value. A file absent from `BASELINE` must be
clean.

## How a call is detected

A direct UEFI call is a PascalCase method invoked through `->`:

    gBS->RaiseTPL(TPL_HIGH_LEVEL)          # flagged
    proto->LocateProtocol(&guid, ...)      # flagged

The wrapped form passes the method as a *bare function pointer* (no call parens),
so it is NOT matched — that is exactly the discriminator:

    axl_efi_call(proto->PassThru, 4, ...)  # NOT flagged (->PassThru, not ->PassThru()

AXL's own vtables use `snake_case` method pointers (`ops->set_pen(...)`), so they
never match the PascalCase pattern. Field reads (`gST->ConOut`, `mode->Attribute`)
are not calls and are not matched.

## Escape hatches

  - EXEMPT_DIRS: `src/backend/`, `src/crt0/` — direct EFI is their whole job.
  - Inline `axl-uefi-direct: <reason>` on the offending line, for a genuine
    one-off with no backend/`axl_efi_call` route.

Exit status is non-zero on a regression, so it can gate CI. Usage:

    scripts/check-dogfood.py                # ratchet gate over src/ + tools/
    scripts/check-dogfood.py --report       # list every site, ignore BASELINE
    scripts/check-dogfood.py --update-baseline  # print a ready-to-paste BASELINE
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# tools/ is scanned alongside src/ because it is code consumers read and copy.
# The rule "AXL dogfoods its own API" was enforced only where the library
# lives, so nothing stopped a tool from demonstrating the raw firmware calls
# the library exists to replace. Widening found 48 of 51 tools/ files already
# clean; the three protocol shims carry real debt and are RECORDED in BASELINE
# below rather than rewritten -- see the note there.
#
# sdk/examples/ is deliberately NOT here yet. It is the other tree consumers
# copy from and it ships in the .deb/.rpm, but two of its files use UEFI
# directly (smbus-hc-shim.c, pointer-tune-demo.c) including two unmarked raw
# pool frees, so adding it is a separate change with its own markers and
# baseline, not a one-line scope bump.
SCAN_ROOTS = (ROOT / "src", ROOT / "tools")
EXTS = {".c", ".h"}


def scan_paths() -> list[Path]:
    """Every candidate path across all scanned roots, in a stable order."""
    out: list[Path] = []
    for root in SCAN_ROOTS:
        out.extend(root.rglob("*"))
    return sorted(out)


# Direct EFI is the *purpose* of these trees, so they are exempt wholesale.
EXEMPT_DIRS = ("src/backend/", "src/crt0/")

# Inline marker on the offending line for a genuine one-off with no backend route.
ALLOW_MARKER = "axl-uefi-direct"

# A PascalCase method invoked through `->` with call parens. AXL vtables are
# snake_case, so this is a UEFI protocol/boot-service call; the wrapped
# `axl_efi_call(p->Method, ...)` form has no call parens and is not matched.
CALL_RE = re.compile(r"->\s*[A-Z][A-Za-z0-9_]*\s*\(")

# ---------------------------------------------------------------------------
# Allocation axis — dogfood axl_malloc/axl_free; every RAW firmware pool call
# must justify itself.
#
# axl_malloc prepends a bookkeeping header, so the pointer it returns is not the
# start of the underlying AllocatePool block. Memory the firmware frees, reads
# at a fixed address, or itself allocated (LocateHandleBuffer, QueryMode,
# GetMemorySpaceMap, Convert*DevicePath*, …) must therefore use raw
# AllocatePool/FreePool — using axl_malloc/axl_free there corrupts the pool
# (CoreFreePool ASSERT / use-after-free). See docs/AXL-Coding-Style.md,
# "Memory Ownership".
#
# So the rule is: default to axl_malloc/axl_free; a raw firmware pool call is
# allowed ONLY with an inline `axl-pool-direct: <reason>` marker naming why the
# memory is firmware-owned. This gate fails on any UNMARKED raw pool call (no
# baseline — mark every legitimate one, convert the rest). Matches both the
# direct `->AllocatePool(` and the wrapped `axl_efi_call(p->FreePool, ...)`
# forms via a word boundary instead of requiring call parens.
POOL_RE = re.compile(r"->\s*(?:AllocatePool|FreePool|AllocatePages|FreePages)\b")
POOL_MARKER = "axl-pool-direct"
# src/mem implements axl_malloc on top of AllocatePages; backend/crt0 bootstrap
# it — all three call the firmware allocator by necessity.
POOL_EXEMPT_DIRS = ("src/backend/", "src/crt0/", "src/mem/")

# Per-file baseline of KNOWN raw-UEFI-call sites (existing debt). A file absent
# here must have ZERO; a file present must not EXCEED its number. Burn down by
# routing the call through axl_efi_call / a backend function and lowering the
# number (run --update-baseline to regenerate this block).
BASELINE: dict[str, int] = {
    "src/fs/axl-fs-provider.c": 1,
    "src/gfx/axl-gfx-output.c": 6,
    "src/gfx/axl-gfx.c": 14,
    "src/hii/axl-hii.c": 27,
    "src/input/axl-input.c": 30,
    "src/input/axl-virtual-pointer.c": 31,
    "src/ipmi/axl-ipmi-dell.c": 2,
    "src/ipmi/axl-ipmi-edkii.c": 3,
    "src/ipmi/axl-ipmi.c": 5,
    "src/log/axl-log.c": 1,
    "src/mem/axl-mem.c": 2,
    "src/net/axl-mbedtls-platform.c": 1,
    "src/net/axl-net-dhcp.c": 4,
    "src/net/axl-udp.c": 17,
    "src/posix/axl-app.c": 2,
    "src/ramdisk/axl-ramdisk.c": 4,
    "src/smbus/axl-smbus-hc.c": 2,
    "src/smbus/axl-smbus-i2c.c": 2,
    "src/smbus/axl-smbus.c": 4,
    "src/util/axl-boot.c": 5,
    "src/util/axl-console-device.c": 46,
    "src/util/axl-console-input.c": 12,
    "src/util/axl-console-tap.c": 23,
    "src/util/axl-cpu.c": 8,
    "src/util/axl-diag.c": 3,
    "src/util/axl-driver-info.c": 45,
    "src/util/axl-driver.c": 51,
    "src/util/axl-image-verify.c": 2,
    "src/util/axl-image.c": 10,
    "src/util/axl-mem-region.c": 8,
    "src/util/axl-nvstore.c": 4,
    "src/util/axl-protocol.c": 2,
    "src/util/axl-rng.c": 2,
    "src/util/axl-shared-driver.c": 2,
    "src/util/axl-shell.c": 3,
    "src/util/axl-sys.c": 5,
    "src/util/axl-watchdog.c": 1,
    # tools/ debt, recorded when the scan was widened to cover it. All three
    # are protocol SHIMS rather than ordinary apps -- kbtune-drv reinstalls
    # SIMPLE_TEXT_INPUT and chains to the original vtable, fbcon-drv publishes
    # a presence GUID, fbcon locates and unloads resident images -- so raw EFI
    # is closer to their purpose than to a drift. Ratcheted, not rewritten:
    # they may not GAIN calls, which is what the widening was for.
    "tools/fbcon-drv.c": 5,
    "tools/fbcon.c": 3,
    "tools/kbtune-drv.c": 27,
}


def blank_noncode(text: str) -> str:
    """Return `text` with comments and string/char literals replaced by spaces
    (newlines preserved, so line numbers are unchanged). Leaves code intact so a
    `->Method(` inside a comment or string is not mistaken for a call."""
    out: list[str] = []
    i, n = 0, len(text)
    state = "code"   # code | line_comment | block_comment | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "\n":
            out.append("\n")
            if state == "line_comment":
                state = "code"
            i += 1
            continue
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block_comment"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
                out.append(" ")
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(" ")
                i += 1
                continue
            out.append(c)
            i += 1
            continue
        if state == "line_comment":
            out.append(" ")
            i += 1
            continue
        if state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append(" ")
            i += 1
            continue
        # string / char
        if c == "\\":
            out.append("  ")
            i += 2
            continue
        if (state == "string" and c == '"') or (state == "char" and c == "'"):
            state = "code"
            out.append(" ")
            i += 1
            continue
        out.append(" ")
        i += 1
    return "".join(out)


def scan_file(path: Path) -> list[tuple[int, str]]:
    """Return (line_no, source_line) for each raw UEFI call in `path`, skipping
    lines carrying the inline allow marker."""
    text = path.read_text(encoding="utf-8", errors="replace")
    code = blank_noncode(text)
    raw_lines = text.splitlines()
    findings: list[tuple[int, str]] = []
    for lineno, code_line in enumerate(code.splitlines(), start=1):
        if not CALL_RE.search(code_line):
            continue
        src = raw_lines[lineno - 1] if lineno - 1 < len(raw_lines) else ""
        if ALLOW_MARKER in src:
            continue
        for _ in CALL_RE.finditer(code_line):
            findings.append((lineno, src.strip()))
    return findings


def is_exempt(rel: str) -> bool:
    return any(rel.startswith(d) for d in EXEMPT_DIRS)


def collect() -> dict[str, list[tuple[int, str]]]:
    """rel-path -> findings, over the non-exempt scan tree."""
    result: dict[str, list[tuple[int, str]]] = {}
    for path in scan_paths():
        if path.suffix not in EXTS or not path.is_file():
            continue
        rel = path.relative_to(ROOT).as_posix()
        if is_exempt(rel):
            continue
        findings = scan_file(path)
        if findings:
            result[rel] = findings
    return result


def scan_file_pool(path: Path) -> list[tuple[int, str]]:
    """Return (line_no, source_line) for each RAW firmware pool call in `path`
    that lacks the axl-pool-direct marker."""
    text = path.read_text(encoding="utf-8", errors="replace")
    code = blank_noncode(text)
    raw_lines = text.splitlines()
    findings: list[tuple[int, str]] = []
    for lineno, code_line in enumerate(code.splitlines(), start=1):
        if not POOL_RE.search(code_line):
            continue
        src = raw_lines[lineno - 1] if lineno - 1 < len(raw_lines) else ""
        if POOL_MARKER in src:
            continue
        findings.append((lineno, src.strip()))
    return findings


def collect_pool() -> dict[str, list[tuple[int, str]]]:
    """rel-path -> unmarked raw pool calls, over the non-exempt scan tree."""
    result: dict[str, list[tuple[int, str]]] = {}
    for path in scan_paths():
        if path.suffix not in EXTS or not path.is_file():
            continue
        rel = path.relative_to(ROOT).as_posix()
        if any(rel.startswith(d) for d in POOL_EXEMPT_DIRS):
            continue
        findings = scan_file_pool(path)
        if findings:
            result[rel] = findings
    return result


def cmd_gate_pool(found: dict[str, list[tuple[int, str]]]) -> int:
    if not found:
        print("check-dogfood[pool]: clean — every raw firmware pool call carries "
              "an axl-pool-direct marker.")
        return 0
    print("check-dogfood[pool]: raw firmware AllocatePool/FreePool/AllocatePages/"
          "FreePages WITHOUT an `axl-pool-direct: <reason>` marker (dogfood "
          "axl_malloc/axl_free, or mark the firmware-owned exception):")
    total = 0
    for rel in sorted(found):
        for lineno, src in found[rel]:
            print(f"  {rel}:{lineno}: {src}")
            total += 1
    print(f'\nFAIL: {total} unmarked raw pool call(s). See docs/AXL-Coding-Style.md '
          '"Memory Ownership".')
    return 1


def cmd_report(found: dict[str, list[tuple[int, str]]]) -> int:
    total = 0
    for rel, findings in found.items():
        print(f"{rel}: {len(findings)}")
        for lineno, src in findings:
            print(f"    {lineno}: {src}")
        total += len(findings)
    print(f"\ntotal raw UEFI calls: {total} in {len(found)} files")
    return 0


def cmd_update_baseline(found: dict[str, list[tuple[int, str]]]) -> int:
    print("BASELINE: dict[str, int] = {")
    for rel in sorted(found):
        print(f'    "{rel}": {len(found[rel])},')
    print("}")
    return 0


def cmd_gate(found: dict[str, list[tuple[int, str]]]) -> int:
    counts = {rel: len(f) for rel, f in found.items()}
    regressions: list[str] = []
    new_files: list[str] = []
    improved: list[str] = []

    for rel, count in sorted(counts.items()):
        base = BASELINE.get(rel)
        if base is None:
            new_files.append(rel)
        elif count > base:
            regressions.append(f"{rel}: {count} raw UEFI calls, baseline {base}")
        elif count < base:
            improved.append(f"{rel}: {count} (baseline {base}) — lower BASELINE to {count}")

    # A baselined file that dropped to zero no longer appears in `found`.
    for rel, base in sorted(BASELINE.items()):
        if rel not in counts:
            improved.append(f"{rel}: 0 (baseline {base}) — remove from BASELINE")

    if new_files:
        print("check-dogfood: raw UEFI protocol/boot-service calls in files with NO "
              "baseline (route through axl_efi_call / a backend fn, or mark the line "
              f"`{ALLOW_MARKER}: <reason>`):")
        for rel in new_files:
            for lineno, src in found[rel]:
                print(f"  {rel}:{lineno}: {src}")
    if regressions:
        print("check-dogfood: raw-UEFI-call count increased over baseline:")
        for r in regressions:
            print(f"  {r}")

    if new_files or regressions:
        print("\nFAIL: new raw UEFI calls. The abstraction keeps every UEFI "
              "touchpoint enumerable — route new calls through the backend.")
        return 1

    if improved:
        print("check-dogfood: clean — and debt was paid down. Update BASELINE:")
        for i in improved:
            print(f"  {i}")
        return 0

    total = sum(counts.values())
    print(f"check-dogfood: clean — {total} known raw UEFI calls in "
          f"{len(counts)} baselined files, none new.")
    return 0


def main(argv: list[str]) -> int:
    found = collect()
    pool = collect_pool()
    if "--report" in argv:
        cmd_report(found)
        print()
        ptotal = sum(len(f) for f in pool.values())
        print(f"unmarked raw pool calls: {ptotal} in {len(pool)} files")
        for rel in sorted(pool):
            for lineno, src in pool[rel]:
                print(f"    {rel}:{lineno}: {src}")
        return 0
    if "--update-baseline" in argv:
        return cmd_update_baseline(found)
    # Both axes gate; combine exit codes so either can fail CI.
    rc_uefi = cmd_gate(found)
    rc_pool = cmd_gate_pool(pool)
    return rc_uefi or rc_pool


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
