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

    non_ascii = [c for c in "\n".join(lines) if ord(c) > 127]
    if non_ascii:
        return fail("the generated tool list is not ASCII: "
                    + ", ".join(sorted(set(repr(c) for c in non_ascii)))
                    + " -- add it to ASCII_FOLD; this file is read with `cat` "
                      "in the UEFI Shell, which draws a block for each.")

    sys.stdout.write("\n".join(lines))
    sys.stdout.write("\n" + TAIL.format(arch=args.arch))
    return 0


TAIL = """PCI name database (consumed by lspci):
  pci-ids.json5    Schema-2 unified file with two
                   independent sections: vendors[] for
                   VID/DID/subsystem names; classes[] for
                   base/subclass/progIF triplets. Curated
                   starter set covering QEMU, common server
                   NICs, NVMe, and GPUs. Bulk-populate from
                   canonical pci.ids via
                   scripts/pci-ids-to-json5.py in the SDK
                   source tree. New class triplets (CXL
                   Memory Expanders etc.) can be added
                   without rebuilding any tool — drop a
                   JSON5 update next to the .efi and lspci
                   picks it up on next launch.

Drivers included (drivers/{arch}/):
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
