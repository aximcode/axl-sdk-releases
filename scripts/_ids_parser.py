#!/usr/bin/env python3
from __future__ import annotations

"""
Shared parser for pci.ids / usb.ids text databases.

Both files share a tab-indented hierarchy from the linux-usb /
pciutils ecosystem:

    # comments
    XXXX  Top-level entry (vendor)             <- column 0
    \\tYYYY  Second-tier entry (device)         <- one tab
    \\t\\tZZZZ WWWW  Third-tier entry             <- two tabs (PCI subsystems)
    C XX  Class entry                          <- 'C ' prefix; class section starts
    \\tYY  Subclass                             <- one tab under C
    \\t\\tZZ  Programming interface              <- two tabs under \\tYY

Differences:
  * PCI's third tier is `SSSS DDDD  Name` (subsystem (svid, sdid)).
    USB's third tier is `II  Name` (interface number under a device)
    and is rarely populated; we drop it on USB.
  * USB's class file extends with `AT`, `HID`, `R`, `BIAS`, `PHY`,
    `HCC`, `VT` taxonomy sections after `C XX`. We don't emit those
    today, but the parser needs to clear class context when one
    appears so subsequent `\\tYY` lines aren't misattributed to the
    last `C XX`.

Two thin frontends consume this module:
  * scripts/pci-ids-to-json5.py — emits PCI schema (id/did/svid/sdid,
    schemas 1 flat / 2 hierarchical, plus optional class overlay)
  * scripts/usb-ids-to-json5.py — emits USB schema (id/pid, schema 1
    hierarchical only — USB has no subsystem dimension)
"""

import re
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Type aliases
# ---------------------------------------------------------------------------

Vendor    = tuple[int, str]                  # (vendor_id, name)
Device    = tuple[int, int, str]             # (vid, did/pid, name)
Subsys    = tuple[int, int, str]             # (svid, sdid, name) — PCI only
ClassBase = tuple[int, str]                  # (base, name)
ClassSub  = tuple[int, int, str]             # (base, sub, name)
ClassProg = tuple[int, int, int, str]        # (base, sub, prog, name)


@dataclass
class ParsedIds:
    """Structured shape returned by parse_ids — the union of fields
    needed by either PCI or USB output. PCI consumers read all fields;
    USB consumers ignore subsystems / subsys_by_device / class_*."""
    vendors:           list[Vendor]                           = field(default_factory=list)
    devices:           list[Device]                           = field(default_factory=list)
    subsystems:        list[Subsys]                           = field(default_factory=list)
    subsys_by_device:  dict[tuple[int, int], list[Subsys]]    = field(default_factory=dict)
    class_bases:       list[ClassBase]                        = field(default_factory=list)
    class_subs:        list[ClassSub]                         = field(default_factory=list)
    class_progs:       list[ClassProg]                        = field(default_factory=list)


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

# Regexes shared across both formats.
_VENDOR_RE     = re.compile(r'^([0-9a-fA-F]{4})\s+(.*?)\s*$')
_DEVICE_RE     = re.compile(r'^\t([0-9a-fA-F]{4})\s+(.*?)\s*$')
_SUBSYS_RE     = re.compile(
    r'^\t\t([0-9a-fA-F]{4})\s+([0-9a-fA-F]{4})\s+(.*?)\s*$'
)
_CLASS_BASE_RE = re.compile(r'^C\s+([0-9a-fA-F]{2})\s+(.*?)\s*$')
_CLASS_SUB_RE  = re.compile(r'^\t([0-9a-fA-F]{2})\s+(.*?)\s*$')
_CLASS_PROG_RE = re.compile(r'^\t\t([0-9a-fA-F]{2})\s+(.*?)\s*$')


def parse_ids(
    text: str,
    *,
    has_subsystems: bool,
    allowed_vendors: set[int] | None = None,
) -> ParsedIds:
    """Parse a pci.ids / usb.ids text dump.

    @p has_subsystems
        True for pci.ids (3rd tier `\\t\\tSSSS DDDD` rows are subsystems
        attached to the parent device). False for usb.ids (3rd tier
        rows are USB interface descriptors and we drop them).

    @p allowed_vendors
        Optional curated subset of top-level IDs to keep. Vendor entries
        themselves are ALWAYS emitted (so name lookups for vendors
        outside the list still resolve); only their devices /
        subsystems are dropped. Class-section entries are global and
        ignore the filter.
    """
    out = ParsedIds()
    current_vendor:     int | None = None
    current_device:     int | None = None
    in_class_section:   bool       = False
    current_class_base: int | None = None
    current_class_sub:  int | None = None

    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue

        # Class-section entry detection. Once `C XX` shows up we never
        # see vendor/device rows again in canonical files (they're
        # monotonic by convention).
        if not in_class_section:
            m = _CLASS_BASE_RE.match(line)
            if m:
                in_class_section = True
                current_vendor = None
                current_device = None
                current_class_base = int(m.group(1), 16)
                current_class_sub  = None
                out.class_bases.append((current_class_base, m.group(2)))
                continue

        if in_class_section:
            m = _CLASS_PROG_RE.match(line)
            if m and current_class_base is not None and current_class_sub is not None:
                prog = int(m.group(1), 16)
                out.class_progs.append(
                    (current_class_base, current_class_sub, prog, m.group(2))
                )
                continue
            m = _CLASS_SUB_RE.match(line)
            if m and current_class_base is not None:
                sub = int(m.group(1), 16)
                current_class_sub = sub
                out.class_subs.append((current_class_base, sub, m.group(2)))
                continue
            m = _CLASS_BASE_RE.match(line)
            if m:
                current_class_base = int(m.group(1), 16)
                current_class_sub  = None
                out.class_bases.append((current_class_base, m.group(2)))
                continue
            # Top-level line that's NOT `C XX` ends the C-section
            # subsection (USB has AT/HID/R/BIAS/PHY/HCC/VT taxonomy
            # sections after C — their `\tYY` entries must not be
            # misattributed to the last `C XX` base). Clear context;
            # stay in class-section mode so vendor/device parsing
            # doesn't restart.
            if not line.startswith('\t'):
                current_class_base = None
                current_class_sub  = None
            continue

        # Vendor / device / subsystem section ----------------------------
        if line.startswith('\t\t'):
            if not has_subsystems:
                # USB: drop interface-number rows (rarely populated;
                # we have no consumer for them).
                continue
            m = _SUBSYS_RE.match(line)
            if m and current_vendor is not None and current_device is not None:
                svid = int(m.group(1), 16)
                sdid = int(m.group(2), 16)
                if allowed_vendors is None or current_vendor in allowed_vendors:
                    entry = (svid, sdid, m.group(3))
                    out.subsystems.append(entry)
                    out.subsys_by_device.setdefault(
                        (current_vendor, current_device), []
                    ).append(entry)
            continue
        if line.startswith('\t'):
            m = _DEVICE_RE.match(line)
            if m and current_vendor is not None:
                did = int(m.group(1), 16)
                current_device = did
                if allowed_vendors is None or current_vendor in allowed_vendors:
                    out.devices.append((current_vendor, did, m.group(2)))
            continue
        m = _VENDOR_RE.match(line)
        if m:
            current_vendor = int(m.group(1), 16)
            current_device = None
            out.vendors.append((current_vendor, m.group(2)))

    return out


# ---------------------------------------------------------------------------
# Shared output helper
# ---------------------------------------------------------------------------

def quote_json5(s: str) -> str:
    """Quote a string for JSON5 single-quoted output. Escape only
    backslashes and the single-quote delimiter; pci.ids / usb.ids
    names are plain ASCII per upstream policy and don't contain
    control chars."""
    return s.replace('\\', '\\\\').replace("'", "\\'")
