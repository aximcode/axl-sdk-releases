#!/usr/bin/env python3
from __future__ import annotations

"""
Convert the canonical pci.ids text database into the JSON5 schemas
that axl-sdk's axl_pci_ids_load and axl_pci_class_load expect.

Source format (https://pci-ids.ucw.cz/):

    # comments start with #
    VVVV  Vendor Name              <- column 0 — vendor
    \\tDDDD  Device Name             <- one tab — device under that vendor
    \\t\\tSSSS DDDD  Subsystem Name   <- two tabs — subsystem under that device
    C XX  Class Name               <- 'C ' prefix — class section starts here
    \\tSS  Subclass Name            <- one tab — subclass under that class
    \\t\\tPP  Programming Interface  <- two tabs — prog_if under that subclass

This extractor produces TWO outputs:

  * pci-ids.json5 (default stdout) — vendors / devices / subsystems
    schema 2 (hierarchical) by default. Use --schema 1 for the legacy
    flat layout.

  * pci-class.json5 (via --emit-class FILE) — base / sub / prog class
    overlay schema 1.

Both files are consumed by the loaders in src/pci/axl-pci-ids.c and
src/pci/axl-pci-class.c.

Usage:
    pci-ids-to-json5.py PCI_IDS_FILE > pci-ids.json5
    pci-ids-to-json5.py --emit-class pci-class.json5 \\
                        PCI_IDS_FILE > pci-ids.json5
    pci-ids-to-json5.py --vendors-only 8086,1022,10de PCI_IDS_FILE
    pci-ids-to-json5.py --self-test    # exercise the parser/emitters

Filter examples (curated subset rather than the whole 6 MB):
    pci-ids-to-json5.py --vendors-only 8086,1022,10de pci.ids \\
        > out.json5

The full pci.ids file generates ~50000 entries (~6 MB JSON5). For a
UEFI tool that's overkill — curated subsets are usually what you
want. The script also accepts stdin (`-` as the input path).
"""

import argparse
import re
import sys
from pathlib import Path
from typing import TextIO


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

# (vendor_id, vendor_name)
Vendor = tuple[int, str]
# (vid, did, device_name)
Device = tuple[int, int, str]
# (svid, sdid, subsystem_name)
Subsys = tuple[int, int, str]
# (base, name)
ClassBase = tuple[int, str]
# (base, sub, name)
ClassSub = tuple[int, int, str]
# (base, sub, prog, name)
ClassProg = tuple[int, int, int, str]

# Output of parse_pci_ids — the structured shape that lets the v2
# emitter properly nest subsystems under their parent device while
# keeping the flat lists for v1 / class-only output.
PciIdsParsed = tuple[
    list[Vendor],
    list[Device],
    list[Subsys],
    dict[tuple[int, int], list[Subsys]],   # subsystems grouped by parent (vid, did)
    list[ClassBase],
    list[ClassSub],
    list[ClassProg],
]


def parse_pci_ids(
    text: str,
    allowed_vendors: set[int] | None = None,
) -> PciIdsParsed:
    """Single-pass parser. Returns:

      vendors             — list of (vid, name)
      devices             — list of (vid, did, name)
      subsystems          — flat list of (svid, sdid, name) for v1 output
      subsys_by_device    — dict {(vid, did): [(svid, sdid, name), ...]}
                            for nested v2 output
      class_bases         — list of (base, name)        from `C XX  ...`
      class_subs          — list of (base, sub, name)   from `\\tSS  ...`
      class_progs         — list of (base, sub, prog, name) from `\\t\\tPP  ...`

    `allowed_vendors` filters devices and subsystems by parent vendor;
    vendor entries themselves are always emitted (so name lookups for
    vendors outside the curated device set still resolve). Class-section
    entries are not affected by the filter — class names are global.
    """
    vendors: list[Vendor] = []
    devices: list[Device] = []
    subsystems: list[Subsys] = []
    subsys_by_device: dict[tuple[int, int], list[Subsys]] = {}
    class_bases: list[ClassBase] = []
    class_subs: list[ClassSub] = []
    class_progs: list[ClassProg] = []

    current_vendor: int | None = None
    current_device: int | None = None
    in_class_section = False
    current_class_base: int | None = None
    current_class_sub: int | None = None

    vendor_re = re.compile(r'^([0-9a-fA-F]{4})\s+(.*?)\s*$')
    device_re = re.compile(r'^\t([0-9a-fA-F]{4})\s+(.*?)\s*$')
    subsys_re = re.compile(
        r'^\t\t([0-9a-fA-F]{4})\s+([0-9a-fA-F]{4})\s+(.*?)\s*$'
    )
    class_base_re = re.compile(r'^C\s+([0-9a-fA-F]{2})\s+(.*?)\s*$')
    class_sub_re  = re.compile(r'^\t([0-9a-fA-F]{2})\s+(.*?)\s*$')
    class_prog_re = re.compile(r'^\t\t([0-9a-fA-F]{2})\s+(.*?)\s*$')

    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue

        # Class section toggle: once we hit `C XX`, all subsequent
        # data is class triplets (no more vendors). Canonical pci.ids
        # is monotonic so the flag never resets.
        if not in_class_section:
            m = class_base_re.match(line)
            if m:
                in_class_section = True
                current_vendor = None
                current_device = None
                current_class_base = int(m.group(1), 16)
                current_class_sub  = None
                class_bases.append((current_class_base, m.group(2)))
                continue

        if in_class_section:
            # Class triplet rows
            m = class_prog_re.match(line)
            if m and current_class_base is not None and current_class_sub is not None:
                prog = int(m.group(1), 16)
                name = m.group(2)
                class_progs.append(
                    (current_class_base, current_class_sub, prog, name)
                )
                continue
            m = class_sub_re.match(line)
            if m and current_class_base is not None:
                sub = int(m.group(1), 16)
                name = m.group(2)
                current_class_sub = sub
                class_subs.append((current_class_base, sub, name))
                continue
            m = class_base_re.match(line)
            if m:
                current_class_base = int(m.group(1), 16)
                current_class_sub  = None
                class_bases.append((current_class_base, m.group(2)))
                continue
            # Anything else in the class section is unrecognized — skip.
            continue

        # Vendor / device / subsystem section
        if line.startswith('\t\t'):
            m = subsys_re.match(line)
            if m and current_vendor is not None and current_device is not None:
                svid = int(m.group(1), 16)
                sdid = int(m.group(2), 16)
                name = m.group(3)
                if allowed_vendors is None or current_vendor in allowed_vendors:
                    entry = (svid, sdid, name)
                    subsystems.append(entry)
                    subsys_by_device.setdefault(
                        (current_vendor, current_device), []
                    ).append(entry)
            continue
        if line.startswith('\t'):
            m = device_re.match(line)
            if m and current_vendor is not None:
                did = int(m.group(1), 16)
                name = m.group(2)
                current_device = did
                if allowed_vendors is None or current_vendor in allowed_vendors:
                    devices.append((current_vendor, did, name))
            continue
        m = vendor_re.match(line)
        if m:
            current_vendor = int(m.group(1), 16)
            current_device = None
            name = m.group(2)
            vendors.append((current_vendor, name))

    return (
        vendors, devices, subsystems, subsys_by_device,
        class_bases, class_subs, class_progs,
    )


# ---------------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------------

def quote_json5(s: str) -> str:
    """Quote a string for JSON5 single-quoted output. Escape only
    backslashes and the single-quote delimiter; pci.ids names are
    plain ASCII per upstream policy and don't contain control chars."""
    return s.replace('\\', '\\\\').replace("'", "\\'")


def _emit_v1_flat(
    vendors: list[Vendor],
    devices: list[Device],
    subsystems: list[Subsys],
    out: TextIO,
) -> None:
    """Schema 1 — three top-level arrays, each entry self-contained."""
    out.write("    schema: 1,\n")
    out.write("    vendors: [\n")
    for vid, name in vendors:
        out.write(f"        {{ id: 0x{vid:04X}, name: '{quote_json5(name)}' }},\n")
    out.write("    ],\n")
    out.write("    devices: [\n")
    for vid, did, name in devices:
        out.write(f"        {{ vid: 0x{vid:04X}, did: 0x{did:04X}, "
                  f"name: '{quote_json5(name)}' }},\n")
    out.write("    ],\n")
    if subsystems:
        out.write("    subsystems: [\n")
        for svid, sdid, name in subsystems:
            out.write(f"        {{ svid: 0x{svid:04X}, sdid: 0x{sdid:04X}, "
                      f"name: '{quote_json5(name)}' }},\n")
        out.write("    ],\n")


def _emit_v2_hierarchical(
    vendors: list[Vendor],
    devices: list[Device],
    subsys_by_device: dict[tuple[int, int], list[Subsys]],
    out: TextIO,
) -> None:
    """Schema 2 — devices nest under their vendor; subsystems nest
    under their parent device. The natural hand-edit shape for
    thousands of entries (locality of related rows; no repeated
    vid: field per device).
    """
    devices_by_vid: dict[int, list[tuple[int, str]]] = {}
    for vid, did, name in devices:
        devices_by_vid.setdefault(vid, []).append((did, name))

    out.write("    schema: 2,\n")
    out.write("    vendors: [\n")
    for vid, vname in vendors:
        v_devs = devices_by_vid.get(vid, [])
        if not v_devs:
            out.write(f"        {{ id: 0x{vid:04X}, "
                      f"name: '{quote_json5(vname)}' }},\n")
            continue
        out.write(f"        {{ id: 0x{vid:04X}, "
                  f"name: '{quote_json5(vname)}',\n")
        out.write(f"          devices: [\n")
        for did, dname in v_devs:
            d_subs = subsys_by_device.get((vid, did), [])
            if not d_subs:
                out.write(f"            {{ did: 0x{did:04X}, "
                          f"name: '{quote_json5(dname)}' }},\n")
                continue
            out.write(f"            {{ did: 0x{did:04X}, "
                      f"name: '{quote_json5(dname)}',\n")
            out.write(f"              subsystems: [\n")
            for svid, sdid, sname in d_subs:
                out.write(f"                {{ svid: 0x{svid:04X}, "
                          f"sdid: 0x{sdid:04X}, "
                          f"name: '{quote_json5(sname)}' }},\n")
            out.write(f"              ],\n")
            out.write(f"            }},\n")
        out.write(f"          ],\n")
        out.write(f"        }},\n")
    out.write("    ],\n")


def emit_pci_ids(
    vendors: list[Vendor],
    devices: list[Device],
    subsystems: list[Subsys],
    subsys_by_device: dict[tuple[int, int], list[Subsys]],
    out: TextIO,
    schema: int = 2,
) -> None:
    """Write the pci-ids.json5 document at the requested schema version."""
    out.write("// Generated by scripts/pci-ids-to-json5.py from pci.ids.\n")
    out.write("// Numeric IDs are uncopyrightable factual data; names are\n")
    out.write("// sourced from the public pci.ids registry.\n")
    out.write("{\n")
    if schema == 1:
        _emit_v1_flat(vendors, devices, subsystems, out)
    elif schema == 2:
        _emit_v2_hierarchical(vendors, devices, subsys_by_device, out)
    else:
        raise ValueError(f"unsupported schema version: {schema}")
    out.write("}\n")


def emit_pci_class(
    bases: list[ClassBase],
    subs: list[ClassSub],
    progs: list[ClassProg],
    out: TextIO,
) -> None:
    """Write the pci-class.json5 overlay document. Schema 1 — single
    top-level classes[] array with each entry pinning any subset of
    (base, sub, prog). The loader (src/pci/axl-pci-class.c) routes
    by tier."""
    out.write("// Generated by scripts/pci-ids-to-json5.py from pci.ids.\n")
    out.write("// Class triplet (base/sub/prog) name overlay — consulted\n")
    out.write("// before the compiled-in tables in src/pci/axl-pci.c.\n")
    out.write("{\n")
    out.write("    schema: 1,\n")
    out.write("    classes: [\n")
    for base, name in bases:
        out.write(f"        {{ base: 0x{base:02X}, "
                  f"name: '{quote_json5(name)}' }},\n")
    for base, sub, name in subs:
        out.write(f"        {{ base: 0x{base:02X}, sub: 0x{sub:02X}, "
                  f"name: '{quote_json5(name)}' }},\n")
    for base, sub, prog, name in progs:
        out.write(f"        {{ base: 0x{base:02X}, sub: 0x{sub:02X}, "
                  f"prog: 0x{prog:02X}, name: '{quote_json5(name)}' }},\n")
    out.write("    ],\n")
    out.write("}\n")


# ---------------------------------------------------------------------------
# Self-test (--self-test mode)
# ---------------------------------------------------------------------------

_SELF_TEST_INPUT = """\
# Sample pci.ids fixture for the script's --self-test mode.
8086  Intel Corporation
\t1521  I350 Gigabit Network Connection
\t\t1028 1F5F  PERC H730 Mini
\t\t1028 1FCA  BCM57416 rNDC
\t100E  82540EM Gigabit Ethernet Controller
10de  NVIDIA
\t1c82  GP107 (GTX 1050 Ti)
0001  EmptyVendor
C 02  Network controller
\t00  Ethernet controller
\t01  Token ring network controller
C 06  Bridge
\t00  Host bridge
\t04  PCI bridge
\t\t01  Subtractive decode
"""


def _self_test() -> int:
    import io

    parsed = parse_pci_ids(_SELF_TEST_INPUT)
    vendors, devices, subsystems, subsys_by_device, \
        class_bases, class_subs, class_progs = parsed

    # Parser shape
    assert vendors == [(0x8086, "Intel Corporation"),
                       (0x10DE, "NVIDIA"),
                       (0x0001, "EmptyVendor")], vendors
    assert devices == [(0x8086, 0x1521,
                        "I350 Gigabit Network Connection"),
                       (0x8086, 0x100E,
                        "82540EM Gigabit Ethernet Controller"),
                       (0x10DE, 0x1C82, "GP107 (GTX 1050 Ti)")], devices
    assert subsystems == [(0x1028, 0x1F5F, "PERC H730 Mini"),
                          (0x1028, 0x1FCA, "BCM57416 rNDC")], subsystems
    assert subsys_by_device == {
        (0x8086, 0x1521): [
            (0x1028, 0x1F5F, "PERC H730 Mini"),
            (0x1028, 0x1FCA, "BCM57416 rNDC"),
        ],
    }, subsys_by_device

    # Class section
    assert class_bases == [(0x02, "Network controller"),
                           (0x06, "Bridge")], class_bases
    assert class_subs == [(0x02, 0x00, "Ethernet controller"),
                          (0x02, 0x01, "Token ring network controller"),
                          (0x06, 0x00, "Host bridge"),
                          (0x06, 0x04, "PCI bridge")], class_subs
    assert class_progs == [(0x06, 0x04, 0x01, "Subtractive decode")], \
        class_progs

    # v2 emit nests subsystems under their parent device.
    buf = io.StringIO()
    emit_pci_ids(vendors, devices, subsystems, subsys_by_device,
                 buf, schema=2)
    out = buf.getvalue()
    assert "schema: 2" in out
    assert "id: 0x8086" in out
    assert "did: 0x1521" in out
    # Subsystem must be NESTED inside the I350 device entry, not in
    # a top-level subsystems[] block.
    nested_marker = "subsystems: [\n                { svid: 0x1028"
    assert nested_marker in out, \
        f"expected nested subsystems block; got:\n{out}"
    # Top-level subsystems[] would appear as its own line with exactly
    # 4-space indent (matching the document's outer object level).
    # Substring check across lines is unreliable because the deeper
    # nested form contains "    subsystems: [" as a non-line-starting
    # substring at column 10. Check line-exact instead.
    for line in out.split("\n"):
        assert line != "    subsystems: [", (
            "v2 should not emit a top-level subsystems[] block when "
            "every subsystem nests cleanly under its parent device"
        )

    # Vendor with no devices renders as a single-line entry (no
    # devices: array, no trailing comma weirdness).
    assert "{ id: 0x0001, name: 'EmptyVendor' }," in out, \
        f"expected single-line vendor-no-devices entry; got:\n{out}"

    # Device with no subsystems renders as a single-line entry inside
    # its vendor's devices[] array (no nested subsystems[] block).
    assert "{ did: 0x1C82, name: 'GP107 (GTX 1050 Ti)' }," in out, \
        f"expected single-line device-no-subsystems entry; got:\n{out}"

    # v1 emit retains the flat layout (back-compat path).
    buf = io.StringIO()
    emit_pci_ids(vendors, devices, subsystems, subsys_by_device,
                 buf, schema=1)
    out = buf.getvalue()
    assert "schema: 1" in out
    assert "vid: 0x8086, did: 0x1521" in out
    assert "svid: 0x1028, sdid: 0x1FCA" in out

    # Class emit produces all three tiers in a single classes[] array.
    buf = io.StringIO()
    emit_pci_class(class_bases, class_subs, class_progs, buf)
    out = buf.getvalue()
    assert "schema: 1" in out
    assert "{ base: 0x06, name: 'Bridge' }" in out
    assert "{ base: 0x06, sub: 0x00, name: 'Host bridge' }" in out
    assert "{ base: 0x06, sub: 0x04, prog: 0x01, " \
           "name: 'Subtractive decode' }" in out

    # Vendor filter affects devices + subsystems but not vendor list.
    parsed_filtered = parse_pci_ids(_SELF_TEST_INPUT, allowed_vendors={0x10DE})
    v2, d2, s2, _, _, _, _ = parsed_filtered
    assert len(v2) == 3, "vendors are unfiltered (Intel + NVIDIA + EmptyVendor)"
    assert len(d2) == 1 and d2[0][0] == 0x10DE, \
        "only NVIDIA devices remain after filter"
    assert s2 == [], "Intel subsystems dropped with their parent vendor"

    print("PASS: scripts/pci-ids-to-json5.py self-test", file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert pci.ids → axl-sdk JSON5 schema",
    )
    parser.add_argument(
        "input", nargs="?",
        help="path to pci.ids file (or '-' for stdin); omit with --self-test",
    )
    parser.add_argument(
        "--vendors-only",
        help="comma-separated hex vendor IDs to include "
             "(e.g. 8086,1022,10de). Vendors not in the list are still "
             "listed; only their devices and subsystems are dropped.",
    )
    parser.add_argument(
        "--schema",
        type=int,
        default=2,
        choices=[1, 2],
        help="pci-ids.json5 schema version (default: 2 hierarchical; "
             "1 = flat legacy)",
    )
    parser.add_argument(
        "-o", "--output",
        help="pci-ids.json5 output file (default: stdout)",
    )
    parser.add_argument(
        "--emit-class",
        metavar="FILE",
        help="also extract class names (C lines) from pci.ids and write "
             "the pci-class.json5 overlay to FILE",
    )
    parser.add_argument(
        "--self-test", action="store_true",
        help="exercise the parser/emitters against an embedded fixture; "
             "exits 0 on pass, non-zero on assertion failure",
    )
    args = parser.parse_args()

    if args.self_test:
        return _self_test()
    if args.input is None:
        parser.error("input file required (or pass --self-test)")

    if args.input == "-":
        text = sys.stdin.read()
    else:
        text = Path(args.input).read_text(encoding="utf-8", errors="replace")

    allowed: set[int] | None = None
    if args.vendors_only:
        allowed = {int(v.strip(), 16)
                   for v in args.vendors_only.split(",") if v.strip()}

    vendors, devices, subsystems, subsys_by_device, \
        class_bases, class_subs, class_progs = parse_pci_ids(text, allowed)

    if args.output:
        with Path(args.output).open("w", encoding="utf-8") as f:
            emit_pci_ids(vendors, devices, subsystems, subsys_by_device,
                         f, schema=args.schema)
    else:
        emit_pci_ids(vendors, devices, subsystems, subsys_by_device,
                     sys.stdout, schema=args.schema)

    if args.emit_class:
        with Path(args.emit_class).open("w", encoding="utf-8") as f:
            emit_pci_class(class_bases, class_subs, class_progs, f)

    print(f"# pci-ids schema {args.schema}: {len(vendors)} vendors, "
          f"{len(devices)} devices, {len(subsystems)} subsystems",
          file=sys.stderr)
    if args.emit_class:
        print(f"# pci-class schema 1: {len(class_bases)} bases, "
              f"{len(class_subs)} subs, {len(class_progs)} progs "
              f"-> {args.emit_class}",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
