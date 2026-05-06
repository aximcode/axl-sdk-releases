#!/usr/bin/env python3
from __future__ import annotations

"""
Convert the canonical pci.ids text database into the JSON5 schema
that axl-sdk's axl_pci_ids_load and axl_pci_class_load expect.

Source format (https://pci-ids.ucw.cz/):

    # comments start with #
    VVVV  Vendor Name              <- column 0 — vendor
    \\tDDDD  Device Name             <- one tab — device under that vendor
    \\t\\tSSSS DDDD  Subsystem Name   <- two tabs — subsystem under that device
    C XX  Class Name               <- 'C ' prefix — class section starts here
    \\tSS  Subclass Name            <- one tab — subclass under that class
    \\t\\tPP  Programming Interface  <- two tabs — prog_if under that subclass

Output (default schema 2):

  pci-ids.json5 — schema 2 hierarchical, ONE file with both
  vendors[] and classes[] blocks. Both axl-sdk loaders read it.
  Either block is optional.

For backward compatibility, --schema 1 emits the legacy flat
vendors-only layout (no classes; consumers that wanted classes used
the separate pci-class.json5 file, which is no longer generated).

The line-level parsing is shared with usb-ids-to-json5.py via
scripts/_ids_parser.py — pci.ids and usb.ids use the same tab-
indented hierarchy.

Usage:
    pci-ids-to-json5.py PCI_IDS_FILE > pci-ids.json5
    pci-ids-to-json5.py --vendors-only 8086,1022,10de PCI_IDS_FILE
    pci-ids-to-json5.py --self-test    # exercise the parser/emitters

The full pci.ids file generates ~50000 entries (~6 MB JSON5). For a
UEFI tool that's overkill — curated subsets are usually what you
want. The script also accepts stdin (`-` as the input path).
"""

import argparse
import sys
from pathlib import Path
from typing import TextIO

from _ids_parser import (
    ClassBase,
    ClassProg,
    ClassSub,
    Device,
    ParsedIds,
    Subsys,
    Vendor,
    parse_ids,
    quote_json5,
)


# ---------------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------------

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
    parsed: ParsedIds,
    out: TextIO,
    schema: int = 2,
) -> None:
    """Write the pci-ids.json5 document at the requested schema version.

    Schema 2 (default) emits BOTH vendors[] and classes[] blocks
    hierarchical in one file — the canonical layout consumed by
    axl_pci_ids_load and axl_pci_class_load.

    Schema 1 emits the legacy flat vendors-only layout for backward
    compatibility. Class data is dropped at this version since
    consumers that wanted classes used a separate pci-class.json5
    file (no longer generated)."""
    out.write("// Generated by scripts/pci-ids-to-json5.py from pci.ids.\n")
    out.write("// Numeric IDs are uncopyrightable factual data; names are\n")
    out.write("// sourced from the public pci.ids registry.\n")
    out.write("{\n")
    if schema == 1:
        _emit_v1_flat(parsed.vendors, parsed.devices, parsed.subsystems, out)
    elif schema == 2:
        _emit_v2_hierarchical(
            parsed.vendors, parsed.devices, parsed.subsys_by_device, out)
        out.write("\n")
        _emit_v2_classes_hierarchical(
            parsed.class_bases, parsed.class_subs, parsed.class_progs, out)
    else:
        raise ValueError(f"unsupported schema version: {schema}")
    out.write("}\n")


def _emit_v2_classes_hierarchical(
    bases: list[ClassBase],
    subs: list[ClassSub],
    progs: list[ClassProg],
    out: TextIO,
    indent: str = "    ",
) -> None:
    """Emit the schema-2 hierarchical classes[] block (subclasses
    nest under bases, progs nest under subclasses).

    The pci.ids `C` lines list base / sub / prog flat — same shape as
    the schema-1 emitter consumes — so we group them locally first.
    A base can have nested subclasses; a sub can have nested progs;
    a base or sub without a name still emits as a parent node when
    it has nested children."""
    base_names = {b: name for b, name in bases}
    # subs[(base, sub)] = sub_name
    sub_map: dict[tuple[int, int], str] = {(b, s): name for b, s, name in subs}
    # progs grouped by (base, sub)
    prog_groups: dict[tuple[int, int], list[tuple[int, str]]] = {}
    for b, s, p, name in progs:
        prog_groups.setdefault((b, s), []).append((p, name))

    # Collect the set of base codes that appear anywhere (named, with
    # subs, or with progs).
    all_bases: set[int] = set(base_names.keys())
    for b, _ in sub_map.keys():
        all_bases.add(b)
    for b, _ in prog_groups.keys():
        all_bases.add(b)

    out.write(f"{indent}classes: [\n")
    for b in sorted(all_bases):
        bname = base_names.get(b)
        # Subclasses for this base
        sub_keys = sorted(s for (bb, s) in sub_map.keys() if bb == b)
        sub_keys += [s for (bb, s) in prog_groups.keys()
                     if bb == b and s not in sub_keys]
        sub_keys = sorted(set(sub_keys))
        if not sub_keys:
            # Base-only entry
            if bname is not None:
                out.write(f"{indent}    {{ base: 0x{b:02X}, "
                          f"name: '{quote_json5(bname)}' }},\n")
            continue
        # Open a base block
        if bname is not None:
            out.write(f"{indent}    {{ base: 0x{b:02X}, "
                      f"name: '{quote_json5(bname)}',\n")
        else:
            out.write(f"{indent}    {{ base: 0x{b:02X},\n")
        out.write(f"{indent}      subclasses: [\n")
        for s in sub_keys:
            sname = sub_map.get((b, s))
            sub_progs = prog_groups.get((b, s), [])
            if not sub_progs:
                if sname is not None:
                    out.write(f"{indent}        {{ sub: 0x{s:02X}, "
                              f"name: '{quote_json5(sname)}' }},\n")
                continue
            if sname is not None:
                out.write(f"{indent}        {{ sub: 0x{s:02X}, "
                          f"name: '{quote_json5(sname)}',\n")
            else:
                out.write(f"{indent}        {{ sub: 0x{s:02X},\n")
            out.write(f"{indent}          progs: [\n")
            for p, pname in sorted(sub_progs):
                out.write(f"{indent}            "
                          f"{{ prog: 0x{p:02X}, "
                          f"name: '{quote_json5(pname)}' }},\n")
            out.write(f"{indent}          ],\n")
            out.write(f"{indent}        }},\n")
        out.write(f"{indent}      ],\n")
        out.write(f"{indent}    }},\n")
    out.write(f"{indent}],\n")


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

    parsed = parse_ids(_SELF_TEST_INPUT, has_subsystems=True)

    # Parser shape
    assert parsed.vendors == [(0x8086, "Intel Corporation"),
                              (0x10DE, "NVIDIA"),
                              (0x0001, "EmptyVendor")], parsed.vendors
    assert parsed.devices == [(0x8086, 0x1521,
                               "I350 Gigabit Network Connection"),
                              (0x8086, 0x100E,
                               "82540EM Gigabit Ethernet Controller"),
                              (0x10DE, 0x1C82, "GP107 (GTX 1050 Ti)")], \
        parsed.devices
    assert parsed.subsystems == [(0x1028, 0x1F5F, "PERC H730 Mini"),
                                 (0x1028, 0x1FCA, "BCM57416 rNDC")], \
        parsed.subsystems
    assert parsed.subsys_by_device == {
        (0x8086, 0x1521): [
            (0x1028, 0x1F5F, "PERC H730 Mini"),
            (0x1028, 0x1FCA, "BCM57416 rNDC"),
        ],
    }, parsed.subsys_by_device

    # Class section
    assert parsed.class_bases == [(0x02, "Network controller"),
                                  (0x06, "Bridge")], parsed.class_bases
    assert parsed.class_subs == [(0x02, 0x00, "Ethernet controller"),
                                 (0x02, 0x01, "Token ring network controller"),
                                 (0x06, 0x00, "Host bridge"),
                                 (0x06, 0x04, "PCI bridge")], parsed.class_subs
    assert parsed.class_progs == [(0x06, 0x04, 0x01, "Subtractive decode")], \
        parsed.class_progs

    # v2 emit nests subsystems under their parent device.
    buf = io.StringIO()
    emit_pci_ids(parsed, buf, schema=2)
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
    emit_pci_ids(parsed, buf, schema=1)
    out = buf.getvalue()
    assert "schema: 1" in out
    assert "vid: 0x8086, did: 0x1521" in out
    assert "svid: 0x1028, sdid: 0x1FCA" in out

    # Schema 2 emit produces both vendors[] and classes[] in one
    # file. Classes are hierarchical: base → subclasses → progs.
    buf = io.StringIO()
    emit_pci_ids(parsed, buf, schema=2)
    out = buf.getvalue()
    assert "schema: 2" in out
    assert "vendors:" in out
    assert "classes:" in out
    assert "subclasses:" in out
    assert "progs:" in out
    assert "{ base: 0x06, name: 'Bridge'" in out
    assert "{ sub: 0x00, name: 'Host bridge'" in out
    assert "{ prog: 0x01, name: 'Subtractive decode' }" in out

    # Vendor filter affects devices + subsystems but not vendor list.
    parsed_filtered = parse_ids(
        _SELF_TEST_INPUT, has_subsystems=True, allowed_vendors={0x10DE})
    assert len(parsed_filtered.vendors) == 3, \
        "vendors are unfiltered (Intel + NVIDIA + EmptyVendor)"
    assert len(parsed_filtered.devices) == 1 \
        and parsed_filtered.devices[0][0] == 0x10DE, \
        "only NVIDIA devices remain after filter"
    assert parsed_filtered.subsystems == [], \
        "Intel subsystems dropped with their parent vendor"

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

    parsed = parse_ids(text, has_subsystems=True, allowed_vendors=allowed)

    if args.output:
        with Path(args.output).open("w", encoding="utf-8") as f:
            emit_pci_ids(parsed, f, schema=args.schema)
    else:
        emit_pci_ids(parsed, sys.stdout, schema=args.schema)

    summary = (f"# pci-ids schema {args.schema}: "
               f"{len(parsed.vendors)} vendors, "
               f"{len(parsed.devices)} devices, "
               f"{len(parsed.subsystems)} subsystems")
    if args.schema == 2:
        summary += (f", {len(parsed.class_bases)} class bases, "
                    f"{len(parsed.class_subs)} subs, "
                    f"{len(parsed.class_progs)} progs")
    print(summary, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
