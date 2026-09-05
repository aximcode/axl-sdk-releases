#!/usr/bin/env python3
"""make-uefi-tools-readme.py -- the README.txt inside the UEFI-tools tarball.

WHY THIS EXISTS. That file said "Tools included:" and listed **13 of 38**.
AXL-Distribution-Design.md §14.3 counted the four places the shipped tool set
is stated: `release.yml`'s sanity list is derived from `make print-TOOL_NAMES`
and `devkit.conf` is gated by `check-devkit-conf`, so both are correct for
free; the two hand-maintained prose lists had each drifted. README.md's copy
was fixed by adding the missing rows behind `make check-tool-docs` -- its
entries are paragraphs, richer than any generator could produce. This one is
different: its entries are flat ~40-character labels, which is exactly what
`devkit.conf`'s `desc:` lines already are, so it is DERIVED.

It was also assembled inline in `release.yml`, so nothing could look at it
without cutting a release. Generating it here gives it the local reproduction
`test/integration/test-uefi-tools-readme.sh` needs -- the same move D2 made
for the host-tools tarball.

THE LICENCE TEXT BELOW IS NOT PROSE. It carries mbedTLS's Apache-2.0
election, EDK2's BSD-2-Clause-Patent notice, iPXE's GPL-2.0-or-later notice
and a GPL-2.0 §3(b) written offer. It was moved here byte-for-byte from the
workflow, and the test asserts each survives, because a refactor that drops a
paragraph of it is a licence violation rather than a typo.

Usage: scripts/make-uefi-tools-readme.py --arch x64|aa64 --version X.Y.Z
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEVKIT_CONF = ROOT / "devkit.conf"
SHARE = ROOT / "share"

# One entry per share/*.json5. The SECTION is derived from what share/ holds,
# not from this table: a sidecar with no entry here fails the generator rather
# than shipping undocumented, and a described file that no longer exists never
# reaches the text. What this replaced described pci-ids.json5 alone -- an
# accurate account of what the tarball staged, and the tarball was missing two.
SIDECAR_DOCS = {
    "pci-ids.json5": (
        "Schema-2 unified file with two independent sections: vendors[] for "
        "VID/DID/subsystem names; classes[] for base/subclass/progIF "
        "triplets. Curated starter set covering QEMU, common server NICs, "
        "NVMe, and GPUs. Bulk-populate from canonical pci.ids via "
        "scripts/pci-ids-to-json5.py in the SDK source tree. New class "
        "triplets (CXL Memory Expanders etc.) can be added without "
        "rebuilding any tool -- drop a JSON5 update next to the .efi and "
        "lspci picks it up on next launch."
    ),
    "usb-ids.json5": (
        "USB vendor and device names. Curated starter set covering common "
        "HID, NIC, hub and storage devices. Bulk-populate from canonical "
        "usb.ids via scripts/usb-ids-to-json5.py in the SDK source tree."
    ),
    "jedec.json5": (
        "JEDEC JEP-106 manufacturer codes, so memspd names a module's vendor "
        "rather than printing a bank/offset pair. Manually curated -- JEP-106 "
        "is not published in a machine-readable form -- covering common "
        "server DRAM vendors."
    ),
}

# Which tool reads which, for the per-file heading.
SIDECAR_CONSUMER = {
    "pci-ids.json5": "lspci",
    "usb-ids.json5": "lsusb",
    "jedec.json5": "memspd",
}

# Width of the `<name>.efi` column, so descriptions line up. Wide enough for
# the longest tool name plus a gap; a name that outgrows it gets one space
# rather than a wrapped column.
NAME_COL = 16
# Total line width. This file ships on a FAT stick beside the .efi tools and is
# read with `cat README.txt` in the UEFI Shell, which is 80 columns and does
# not reflow.
WRAP = 78

# devkit.conf's descriptions are written for a host editor and contain em
# dashes. The UEFI console has no such glyph and draws a block, so the DERIVED
# column is folded to ASCII. The static text below is left exactly as it
# shipped -- it carries licence citations, and rewriting `§3(b)` to make a
# console happier is not a call this script should make.
ASCII_FOLD = {
    "\u2014": "--", "\u2013": "-", "\u2018": "'", "\u2019": "'",
    "\u201c": '"', "\u201d": '"', "\u2026": "...", "\u00a0": " ",
    # Spelled out, not dropped: this one appears inside iPXE's GPL-2.0
    # section 3(b) written offer, where legibility at a UEFI Shell prompt is
    # the whole point of carrying the notice.
    "\u00a7": "section ",
}


def to_ascii(text: str) -> str:
    for bad, good in ASCII_FOLD.items():
        text = text.replace(bad, good)
    return text


def fail(msg: str) -> int:
    print(f"make-uefi-tools-readme: {msg}", file=sys.stderr)
    return 1


def tool_names() -> list[str] | None:
    """The shipped tool set, read back from the Makefile.

    Captures the exit status rather than reading the output alone: a make that
    could not RUN and a make that named nothing are the same empty string and
    opposite facts.
    """
    proc = subprocess.run(["make", "-s", "print-TOOL_NAMES"], cwd=ROOT,
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        fail(f"`make -s print-TOOL_NAMES` exited {proc.returncode}: "
             f"{proc.stderr.strip()}")
        return None
    names = sorted(proc.stdout.split())
    if len(names) < 25:
        fail(f"print-TOOL_NAMES named only {len(names)} tools; refusing to "
             "certify a tarball README against that")
        return None
    return names


def descriptions() -> dict[str, str]:
    """`desc: <DisplayName> <text>` from devkit.conf, keyed by tool name.

    `check-devkit-conf` already requires a line per staged tool, so this map
    has an owner with a gate on it -- which is the whole reason §14.3 chose
    devkit.conf as the source rather than a fifth hand-written list.
    """
    out: dict[str, str] = {}
    for m in re.finditer(r"^desc:\s+(\S+)\s+(.*?)\s*$",
                         DEVKIT_CONF.read_text(encoding="utf-8"), re.M):
        out[m.group(1).lower()] = m.group(2)
    return out


# Returned instead of a filename list when share/ itself is empty -- a
# different fact from "this file has no description", and a different fix.
EMPTY_SHARE: list[str] = []


def sidecar_section() -> str | list[str]:
    """Render the sidecar block from share/, or name the files it cannot.

    Returns the text on success; ``EMPTY_SHARE`` if share/ holds no sidecars
    at all; otherwise the list of filenames with no ``SIDECAR_DOCS`` entry.
    share/ is the authority: this cannot describe a file the tarball does not
    carry, and cannot stay silent about one it does.
    """
    files = sorted(f.name for f in SHARE.glob("*.json5"))
    if not files:
        # Distinct from "undocumented": an empty share/ is share/ VANISHING,
        # and telling the reader to write a description for a file that does
        # not exist sends them after the wrong thing.
        return EMPTY_SHARE
    undocumented = [f for f in files if f not in SIDECAR_DOCS]
    if undocumented:
        return undocumented

    out: list[str] = ["Name databases (auto-discovered next to the .efi):"]
    for name in files:
        consumer = SIDECAR_CONSUMER.get(name)
        out.append(f"  {name}" + (f"    (consumed by {consumer})"
                                  if consumer else ""))
        out.extend(textwrap.wrap(SIDECAR_DOCS[name], width=WRAP,
                                 initial_indent="    ",
                                 subsequent_indent="    "))
        out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--arch", required=True, choices=("x64", "aa64"),
                    help="TARGET firmware architecture, not the host")
    ap.add_argument("--version", required=True)
    args = ap.parse_args()

    names = tool_names()
    if names is None:
        return 1
    desc = descriptions()

    missing = [t for t in names if t not in desc]
    if missing:
        return fail(
            "no `desc:` line in devkit.conf for: " + ", ".join(missing) +
            "\n  `make check-devkit-conf` is the gate that should have caught "
            "this first; a tool with no description would land in the tarball "
            "README as a bare name.")

    lines = [f"AXL SDK pre-built UEFI tools for {args.arch}",
             f"version: {args.version}",
             "",
             "Tools included:"]
    for t in names:
        body = to_ascii(desc[t])
        # Hanging indent under the description column, so a long entry stays
        # readable at 80 columns instead of running off the console.
        wrapped = textwrap.wrap(body, width=WRAP - 2 - NAME_COL) or [""]
        lines.append(f"  {t + '.efi':<{NAME_COL}}{wrapped[0]}")
        for cont in wrapped[1:]:
            lines.append(" " * (2 + NAME_COL) + cont)
    lines.append("")

    sidecars = sidecar_section()
    if isinstance(sidecars, list):
        if not sidecars:
            return fail(f"{SHARE} holds no *.json5 -- the tools tarball ships "
                        "the name databases from there, so this would publish "
                        "lspci, lsusb and memspd with no names at all.")
        return fail("share/*.json5 with no SIDECAR_DOCS entry: "
                    + ", ".join(sidecars)
                    + " -- the tarball ships every share/*.json5, so add a "
                      "description rather than shipping one undocumented.")

    # ONE document, folded ONCE, checked ONCE -- and in that order, so the
    # check sees exactly the bytes that will be written.
    #
    # This guard used to run over the derived tool list alone, which is a third
    # of the file: the hand-written TAIL shipped em-dashes and a section sign
    # for as long as it existed, and SIDECAR_DOCS is hand-written English prose
    # in a .py -- the likeliest source of the next one. Folding without
    # re-checking is the same hole one step along: any character absent from
    # ASCII_FOLD passes through silently. `cat README.txt` at the UEFI Shell
    # draws a block for each, so this is legibility, not pedantry.
    document = to_ascii("\n".join(lines) + "\n" + sidecars + "\n"
                        + TAIL.format(arch=args.arch))
    non_ascii = [c for c in document if ord(c) > 127]
    if non_ascii:
        return fail("the generated README is not ASCII: "
                    + ", ".join(sorted(set(repr(c) for c in non_ascii)))
                    + " -- add it to ASCII_FOLD; this file is read with `cat` "
                      "in the UEFI Shell, which draws a block for each.")

    sys.stdout.write(document)
    return 0


TAIL = """Drivers included (drivers/{arch}/):
  ipxe-all.efidrv     Universal NIC driver — covers Intel,
                      Broadcom, Realtek PCI/USB, Atheros, 3Com,
                      AMD, USB CDC-ECM/NCM/RNDIS, and many more.
  (also a few small auxiliary USB-network and RAM-disk
   drivers — see third_party/ for full attribution.)

Usage:
  1. Format a USB stick as FAT32.
  2. Copy all .efi files AND the drivers/ directory to the stick.
  3. Boot to the UEFI Shell and cd to the stick.
  4. Run each tool with --help for options.

On firmware that already publishes EFI_SIMPLE_NETWORK_PROTOCOL
for the NIC, the staged drivers are unused. On minimal firmware
(legacy Dell EDK1, custom BMCs, etc.) axl_net_ensure_drivers
loads them on demand from drivers/{arch}/ before networking
tools (netinfo, fetch, rfbrowse) attempt to use the network.

TLS / HTTPS:
  Built with mbedtls; fetch handles both http:// and
  https:// URLs, and rfbrowse (Redfish) is fully
  functional. Tools that don't reference networking
  (mkrd, hexdump, find, grep, sysinfo) link without
  pulling mbedtls in, so binary sizes stay minimal.

Third-party licenses:
  mbedtls is Copyright (c) The Mbed TLS Contributors,
  dual-licensed Apache-2.0 OR GPL-2.0-or-later; this
  distribution elects Apache-2.0. Full license text at
  third_party/mbedtls/LICENSE.

  EDK2 drivers (RamDiskDxe, NetworkCommon, UsbCdc{{Ecm,Ncm}},
  UsbRndis) are Copyright (c) Intel Corporation, licensed
  BSD-2-Clause-Patent (from the EDK2 MdeModulePkg). Full
  license text at third_party/edk2/LICENSE.

  ipxe-all.efidrv is built from upstream iPXE
  (https://github.com/ipxe/ipxe), licensed GPL-2.0-or-later.
  Pinned commit + reproducible build recipe at
  scripts/build-ipxe.sh in the axl-sdk source tree. Full
  license text at third_party/ipxe/COPYING.GPLv2; mere-
  aggregation rationale in third_party/ipxe/README.md.

  GPL-2.0 §3(b) written offer: AximCode offers, for at
  least 3 years from this distribution date, to provide
  the complete machine-readable source for the iPXE
  binary in this archive on a medium customarily used
  for software interchange, at no more than the reasonable
  cost of physical distribution. Requests:
    support@aximcode.com  /  https://aximcode.com
  (The source is also publicly available at the upstream
  URL above at the pinned commit hash printed by
  scripts/build-ipxe.sh.)

Docs: https://axl.aximcode.com/
"""


if __name__ == "__main__":
    sys.exit(main())
