#!/usr/bin/env python3
"""Generate an AxlFont C source file from a BDF (Bitmap Distribution Format) font.

Parses a BDF file, optionally restricts to a codepoint allowlist, and emits a
C source file that defines an `AxlGlyph[]` array and a `const AxlFont`
matching the axl-font.h ABI.

The BDF file is NOT committed to the repository — only the generated .c file
is.  To regenerate (or extend) a font:

    # Download Unifont (SIL OFL 1.1 / GPLv2+ with Font Embedding Exception):
    curl -sSLO https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.bdf.gz
    gunzip unifont-16.0.04.bdf.gz

    # Generate the AGT-targeted subset:
    python3 scripts/gen-bdf-font.py \\
        --bdf unifont-16.0.04.bdf \\
        --name unifont_16 \\
        --ascent 14 --descent 2 \\
        --subset 0x20-0x7E,0xA0-0xFF,0x2500-0x257F,0x2580-0x259F,0x2190-0x21AF \\
        --extra 0x2022,0x2026,0x2713,0x2717,0x2009 \\
        --description "GNU Unifont 16.0.04 (subset: ASCII+Latin-1+box+block+arrows)" \\
        --license SIL-OFL-1.1 \\
        --output src/gfx/fonts/font-unifont-16.c

The output file's SPDX header reflects --license; the generator is upstream-
agnostic.  axl-sdk currently ships SIL OFL 1.1 (Unifont) and BSD-2-Clause-Patent
(EDK2 LaffStd) as the two built-in fonts.

Cell height in the output is taken from the BDF FONTBOUNDINGBOX; ascent +
descent must equal cell_height (enforced).
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class BdfGlyph:
    codepoint: int
    width: int          # BBX width
    height: int         # BBX height
    bdf_x_offset: int   # BBX x-origin (positive = right of pen)
    bdf_y_offset: int   # BBX y-origin (positive = above baseline)
    advance: int        # DWIDTH x
    bitmap: list[int] = field(default_factory=list)  # one int per row, MSB-left


def parse_bdf(path: Path) -> tuple[dict, list[BdfGlyph]]:
    """Return (font_meta, glyphs). font_meta has 'cell_width', 'cell_height',
    'bdf_descent'. Glyphs are returned in encoding order."""
    meta: dict = {}
    glyphs: list[BdfGlyph] = []
    cur: BdfGlyph | None = None
    in_bitmap = False
    bitmap_rows: list[int] = []
    bitmap_byte_width = 0  # bytes per row in BDF for cur glyph

    with path.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip()
            if line.startswith("FONTBOUNDINGBOX "):
                # FONTBOUNDINGBOX width height xoff yoff
                _, w, h, _xo, yo = line.split()
                meta["cell_width"] = int(w)
                meta["cell_height"] = int(h)
                meta["bdf_descent"] = -int(yo)  # BDF yoff is negative for descent
            elif line.startswith("STARTCHAR"):
                cur = BdfGlyph(0, 0, 0, 0, 0, 0)
                in_bitmap = False
                bitmap_rows = []
                bitmap_byte_width = 0
            elif line.startswith("ENCODING ") and cur is not None:
                cur.codepoint = int(line.split()[1])
            elif line.startswith("DWIDTH ") and cur is not None:
                # DWIDTH x y — we use x only
                cur.advance = int(line.split()[1])
            elif line.startswith("BBX ") and cur is not None:
                # BBX width height xoff yoff
                _, w, h, xo, yo = line.split()
                cur.width = int(w)
                cur.height = int(h)
                cur.bdf_x_offset = int(xo)
                cur.bdf_y_offset = int(yo)
                bitmap_byte_width = (cur.width + 7) // 8
            elif line == "BITMAP":
                in_bitmap = True
            elif line == "ENDCHAR" and cur is not None:
                cur.bitmap = bitmap_rows
                glyphs.append(cur)
                cur = None
                in_bitmap = False
            elif in_bitmap and cur is not None:
                # Hex row, MSB-first within each byte, padded to byte boundary.
                row_val = int(line, 16)
                bitmap_rows.append(row_val)

    if "cell_width" not in meta:
        sys.exit(f"{path}: missing FONTBOUNDINGBOX")
    return meta, glyphs


def parse_codepoint(s: str) -> int:
    s = s.strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def parse_subset(spec: str) -> set[int]:
    """Parse 'A-B,C-D,...' or 'A,B,C-D' into a set of codepoints."""
    if not spec:
        return set()
    cps: set[int] = set()
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" in chunk:
            a, b = chunk.split("-", 1)
            cps.update(range(parse_codepoint(a), parse_codepoint(b) + 1))
        else:
            cps.add(parse_codepoint(chunk))
    return cps


def emit_glyph_bitmap(glyph: BdfGlyph) -> str:
    """Format glyph's row bytes as a 32-byte AXL_GLYPH_MAX_BYTES array.
    BDF stores each row as a single integer (MSB-left, padded to byte boundary).
    We convert to byte-array form: high byte first per row, height*stride bytes."""
    if len(glyph.bitmap) != glyph.height:
        sys.exit(
            f"glyph U+{glyph.codepoint:04X}: bitmap rows {len(glyph.bitmap)} "
            f"!= BBX height {glyph.height} (malformed BDF input)"
        )
    stride = (glyph.width + 7) // 8
    needed = stride * glyph.height
    if needed > 32:
        sys.exit(
            f"glyph U+{glyph.codepoint:04X}: bitmap {needed} bytes exceeds "
            f"AXL_GLYPH_MAX_BYTES=32 (width={glyph.width}, height={glyph.height})"
        )
    bytes_out: list[int] = []
    for row_val in glyph.bitmap:
        # row_val occupies stride bytes, MSB on the left
        for byte_idx in range(stride):
            shift = (stride - 1 - byte_idx) * 8
            bytes_out.append((row_val >> shift) & 0xFF)
    # Pad to 32 bytes
    while len(bytes_out) < 32:
        bytes_out.append(0)
    return "{ " + ", ".join(f"0x{b:02X}" for b in bytes_out) + " }"


SPDX_TEMPLATE_HEAD = """\
/* SPDX-License-Identifier: {license} */
/*
 * {description}
 *
 * Generated by scripts/gen-bdf-font.py from {bdf_source}.
 * Do not edit by hand.  Source license obligations preserved below.
 *
{license_text}
 */

#include <axl/axl-font.h>
"""

LICENSE_NOTICES = {
    "SIL-OFL-1.1": """\
 * Upstream font:    GNU Unifont (https://unifoundry.com/unifont/)
 * Upstream license: SIL Open Font License 1.1 (https://openfontlicense.org/)
 *                   AND GPLv2+ with the GNU Font Embedding Exception.
 *                   axl-sdk uses the SIL OFL 1.1 licensing path.
 *
 * SIL OFL 1.1 obligations satisfied here:
 *   - Original copyright notice is preserved (see Roman Czyborra et al.).
 *   - This file is the only source distribution unit for the embedded glyphs.
 *   - "Unifont" is a Reserved Font Name; this subset retains the name as it
 *     is a literal extraction without modification to glyph data.
 *
 * Original copyright:
 *   Copyright (C) 1998-2025 Roman Czyborra, Paul Hardy, Qianqian Fang,
 *   Andrew Miller, Johnnie Weaver, David Corbett, Nils Moskopp,
 *   Rebecca Bettencourt, Minseo Lee, Ho-Seok Ee, et al.""",
    "BSD-2-Clause-Patent": " * Upstream license: BSD-2-Clause-Patent (EDK2)",
    "Apache-2.0": " * License: Apache-2.0",
}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bdf",          required=True, help="path to BDF file")
    p.add_argument("--name",         required=True, help="C identifier base (e.g. unifont_16)")
    p.add_argument("--description",  required=True, help="human-readable description")
    p.add_argument("--ascent",       type=int, required=True, help="pixels above baseline")
    p.add_argument("--descent",      type=int, required=True, help="pixels below baseline")
    p.add_argument("--subset",       default="",  help="comma-separated codepoint ranges (e.g. 0x20-0x7E,0xA0-0xFF)")
    p.add_argument("--extra",        default="",  help="comma-separated extra single codepoints to include")
    p.add_argument("--license",      required=True, help="SPDX license id for generated file (e.g. SIL-OFL-1.1, BSD-2-Clause-Patent)")
    p.add_argument("--fallback",     default="0", help="codepoint to render when a glyph is missing (default 0 = blank; use 0x3F for '?')")
    p.add_argument("--output",       required=True, help="output .c path")
    args = p.parse_args()

    bdf_path = Path(args.bdf)
    out_path = Path(args.output)
    meta, all_glyphs = parse_bdf(bdf_path)

    if args.ascent + args.descent != meta["cell_height"]:
        sys.exit(
            f"--ascent ({args.ascent}) + --descent ({args.descent}) != "
            f"BDF cell_height ({meta['cell_height']})"
        )

    allow = parse_subset(args.subset) | parse_subset(args.extra)
    glyphs = [g for g in all_glyphs if g.codepoint in allow] if allow else all_glyphs

    glyphs.sort(key=lambda g: g.codepoint)
    if not glyphs:
        sys.exit("no glyphs matched the subset spec")

    out_path.parent.mkdir(parents=True, exist_ok=True)

    license_text = LICENSE_NOTICES.get(args.license, f" * License: {args.license}")
    lines: list[str] = [SPDX_TEMPLATE_HEAD.format(
        license=args.license,
        description=args.description,
        bdf_source=bdf_path.name,
        license_text=license_text,
    )]
    lines.append("")
    lines.append("/* Glyphs MUST stay sorted by ascending codepoint —")
    lines.append(" * axl_font_glyph() does binary search.  The generator sorts;")
    lines.append(" * hand-edits to this file must preserve the invariant. */")
    lines.append(f"static const AxlGlyph {args.name}_glyphs[] = {{")

    for g in glyphs:
        # Convert BDF y-offset to axl-font convention:
        #   y_offset_axl = ascent - BDF_y_offset - height  (positive = down from cell top)
        y_off_axl = args.ascent - g.bdf_y_offset - g.height
        # Clamp signed int8 ranges
        if not (-128 <= y_off_axl <= 127) or not (-128 <= g.bdf_x_offset <= 127):
            sys.exit(f"U+{g.codepoint:04X}: offset out of int8 range")
        if not (0 <= g.width <= 255) or not (0 <= g.height <= 255) or not (0 <= g.advance <= 255):
            sys.exit(f"U+{g.codepoint:04X}: dimension out of uint8 range")
        bitmap = emit_glyph_bitmap(g)
        try:
            ch = chr(g.codepoint)
            ch_comment = f"'{ch}'" if ch.isprintable() and ch != "'" else f"U+{g.codepoint:04X}"
        except ValueError:
            ch_comment = f"U+{g.codepoint:04X}"
        lines.append(
            f"    {{ 0x{g.codepoint:04X}, {g.width}, {g.height}, "
            f"{g.bdf_x_offset}, {y_off_axl}, {g.advance}, "
            f"{{0,0,0}}, {bitmap} }},  /* {ch_comment} */"
        )

    lines.append("};")
    lines.append("")
    lines.append(f"const AxlFont axl_font_{args.name} = {{")
    lines.append(f'    .name              = "{args.name.replace("_", "-")}",')
    lines.append(f'    .description       = "{args.description}",')
    # All Unifont glyphs are monospace within their respective half/full cell.
    # Mark as VARIABLE since glyph widths differ (8 for half, 16 for full).
    lines.append("    .flags             = AXL_FONT_VARIABLE,")
    lines.append(f"    .cell_width        = {meta['cell_width']},")
    lines.append(f"    .cell_height       = {meta['cell_height']},")
    lines.append(f"    .ascent            = {args.ascent},")
    lines.append(f"    .descent           = {args.descent},")
    lines.append(f"    .line_height       = {meta['cell_height']},")
    lines.append(f"    .n_glyphs          = sizeof({args.name}_glyphs) / sizeof({args.name}_glyphs[0]),")
    lines.append(f"    .glyphs            = {args.name}_glyphs,")
    fallback_cp = parse_codepoint(args.fallback)
    fallback_comment = "no fallback" if fallback_cp == 0 else f"render as U+{fallback_cp:04X} on miss"
    lines.append(f"    .fallback_codepoint = 0x{fallback_cp:04X},  /* {fallback_comment} */")
    lines.append("};")
    lines.append("")

    out_path.write_text("\n".join(lines))
    print(f"Wrote {out_path}: {len(glyphs)} glyphs, cell {meta['cell_width']}x{meta['cell_height']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
