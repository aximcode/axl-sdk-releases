#!/usr/bin/env python3
"""check-uefi-scope.py — keep UEFI types out of the places that promise not to
have them.

AXL's headline contract is that consumers write standard C against `axl_*` and
never see EDK2. Nothing enforced it. `uefi/` ships INSIDE `include/axl-sdk/`,
which `axl-cc` puts on `-isystem`, so `#include <uefi/axl-uefi.h>` has always
worked for every consumer, app or driver -- and `check-dogfood` cannot see it,
because that gate matches CALLS (`->PascalCase(`), not types, constants or
includes.

Two rules, deliberately different in strength:

  1. PUBLIC API -- `include/axl/` must contain ZERO UEFI references in code.
     No allowlist, no baseline. It is at zero today, which is the cheapest
     moment to lock it: this gate starts green and can only ever catch a
     regression. This is the contract CLAUDE.md states ("Standard C types in
     public API (never UEFI types)", "No EDK2 headers leak through the public
     API") and it was previously true by discipline alone.

  2. SHIPPED EXAMPLE CODE -- a file under `tools/` or `sdk/examples/` may use
     UEFI only if it is in ALLOWED below WITH a reason. That does not forbid
     it: a protocol shim cannot avoid the protocol's own types. It forbids it
     happening SILENTLY. `rg EFI_ tools` returning a wall of hits should mean
     "these six declared it", not "who knows".

This gate is the SECOND of two layers, and only the second one covers this
repo. The first is structural and covers consumers: `<uefi/axl-uefi.h>` now
refuses to compile unless `AXL_ALLOW_UEFI` is defined, which `axl-cc` grants
to `--type driver` and to an explicit `--allow-uefi`, and the generated
axl-config.cmake grants to `axl_add_driver` and to `ALLOW_UEFI`. That stops an
application acquiring EDK2 by typing an `#include`.

All THREE doorways carry the guard: `axl-uefi.h`, the hand-written
`axl-uefi-extra.h`, and the generated `all.h` -- that last one emitted by
scripts/generate-uefi-headers.py rather than hand-edited, so it survives the
next regeneration instead of being silently dropped.

What remains uncovered is a file including one LEAF generated header directly
(`<uefi/generated/types.h>`). Guarding every leaf would put an #error above
every type definition for no extra safety, since reaching a leaf that way is
already a deliberate act. That is this gate's job: it matches ANY `uefi/`
include plus the symbol spellings, across this repo's own shipped code, which
is exactly where such a bypass would show up.

Comments and string literals are stripped before matching (reusing
check-dogfood's blanker), because prose ABOUT a protocol is not use of it --
counting raw grep hits overstates this by roughly 2x.
"""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

_spec = importlib.util.spec_from_file_location(
    "_dogfood", ROOT / "scripts" / "check-dogfood.py"
)
if _spec is None or _spec.loader is None:      # pragma: no cover
    sys.exit("check-uefi-scope: cannot load check-dogfood.py for its blanker")
_dogfood = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_dogfood)
blank_noncode = _dogfood.blank_noncode

EXTS = {".c", ".h", ".cpp", ".hpp"}

# A UEFI spelling: EFI_*/EFIAPI, the TPL_/EVT_ constant families, and the
# EDK2 scalar spellings AXL's public API replaces with stdint types.
SYM_RE = re.compile(
    r"\b(?:EFI[A-Z0-9_]*|TPL_[A-Z_]+|EVT_[A-Z_]+"
    r"|BOOLEAN|UINTN|INTN|CHAR16)\b"
)
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]uefi/", re.MULTILINE)

# Rule 1: zero tolerance, no escape hatch.
#
# include/axl.h is a sibling FILE of include/axl/, not inside it, so scanning
# only the directory missed the single most-included public header -- the one
# place a regression would do the most damage. include/compat/ used to be a
# second root (it shipped, and axl-cc put it on -isystem); it is gone, and a
# root that does not exist scans nothing while the gate still reports clean.
PUBLIC_API_ROOTS = (ROOT / "include" / "axl",)
PUBLIC_API_FILES = (ROOT / "include" / "axl.h",)

# Rule 2: declared exceptions. A file here still shows up in `rg EFI_`, but now
# the answer to "why" is in the tree instead of in someone's memory.
ALLOWED: dict[str, str] = {
    "tools/kbtune-drv.c":
        "driver: implements the EFI_SIMPLE_TEXT_INPUT/_EX vtables it "
        "interposes on -- a protocol shim cannot avoid the protocol's types",
    "tools/fbcon-drv.c":
        "driver: publishes a presence GUID and reads EFI_KEY_DATA on the "
        "hotkey path",
    "tools/fbcon-marker.h":
        "the EFI_GUID literal the fbcon driver publishes and fbcon looks up",
    "tools/fbcon.c":
        "unloads resident images found by GUID; axl_image_unload releases an "
        "image AXL itself loaded, not one located by handle",
    "tools/mkfixture.c":
        "emits binary fixtures that must match spec-defined layouts "
        "(EFI_SYSTEM_RESOURCE_TABLE, EFI_MAC_ADDRESS) byte for byte",
    "tools/mkrd.c":
        "one GUID, for the -v probe reporting whether the optional "
        "EFI_RAM_DISK_PROTOCOL is present; the work goes through AxlRamDisk",
    "tools/netload.c":
        "deliberate EFI_SIMPLE_NETWORK_PROTOCOL seam kept for the --_hmap "
        "root-cause repro",
    "tools/crashtest.c":
        "faults on purpose to exercise the crash handler; it needs the raw "
        "types to build the bad state",
    "sdk/examples/smbus-hc-shim.c":
        "DXE driver that PUBLISHES EFI_I2C_MASTER_PROTOCOL over QEMU's ICH9 "
        "SMBus controller, so stock OVMF can reach the emulated smbus-ipmi "
        "device; producing a protocol means implementing its types",
    "sdk/examples/pointer-tune-demo.c":
        "reads EFI_SIMPLE_POINTER_MODE off the raw protocol to display the "
        "resolution AxlInput normalises away -- the whole point of the demo "
        "is showing what the abstraction is doing to the hardware values",
}

SCANNED_ROOTS = (ROOT / "tools", ROOT / "sdk" / "examples")


def uses_uefi(path: Path) -> tuple[int, bool]:
    """(symbol references in code, includes a uefi/ header)."""
    code = blank_noncode(path.read_text(errors="replace"))
    return len(SYM_RE.findall(code)), bool(INCLUDE_RE.search(code))


def iter_sources(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*") if p.suffix in EXTS and p.is_file())


def main() -> int:
    errors: list[str] = []

    # Rule 1 — the public API carries no UEFI at all.
    leaked: list[str] = []
    public: list[Path] = [f for f in PUBLIC_API_FILES if f.is_file()]
    for root in PUBLIC_API_ROOTS:
        if root.is_dir():
            public.extend(iter_sources(root))
    for path in sorted(public):
        n, inc = uses_uefi(path)
        if n or inc:
            rel = path.relative_to(ROOT).as_posix()
            leaked.append(f"{rel}: {n} UEFI reference(s)"
                          f"{', includes <uefi/...>' if inc else ''}")
    if leaked:
        errors.append(
            "PUBLIC API must contain no UEFI types or includes -- consumers "
            "are promised standard C:\n    " + "\n    ".join(leaked)
        )

    # Rule 2 — shipped example code declares its UEFI use.
    undeclared: list[str] = []
    stale: list[str] = []
    seen: set[str] = set()
    for root in SCANNED_ROOTS:
        if not root.is_dir():
            continue
        for path in iter_sources(root):
            rel = path.relative_to(ROOT).as_posix()
            n, inc = uses_uefi(path)
            if n or inc:
                seen.add(rel)
                if rel not in ALLOWED:
                    undeclared.append(f"{rel}: {n} UEFI reference(s)"
                                      f"{', includes <uefi/...>' if inc else ''}")
    for rel in ALLOWED:
        if rel not in seen:
            stale.append(rel)

    if undeclared:
        errors.append(
            "shipped example code uses UEFI without declaring it -- add it to "
            "ALLOWED in this script WITH a reason, or route it through an "
            "axl_* API:\n    " + "\n    ".join(undeclared)
        )
    if stale:
        # A stale entry is not cosmetic: it is a standing permission for a file
        # that no longer needs one, and the next file to need it inherits an
        # unexamined "yes".
        errors.append(
            "ALLOWED lists files that no longer use UEFI -- remove them so the "
            "list keeps meaning something:\n    " + "\n    ".join(sorted(stale))
        )

    if errors:
        print("check-uefi-scope: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1

    print(f"check-uefi-scope: clean — public API has zero UEFI references; "
          f"{len(seen)} declared exception(s) in shipped example code.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
