#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""gop-cells.py -- which character cells of a GOP text console carry ink.

Reads a QEMU `screendump` (binary PPM, P6) of a screen the firmware's
GraphicsConsole draws: a COLS x ROWS grid of CELL_W x CELL_H glyph cells,
centred in the framebuffer as GraphicsConsole centres it. Prints one line per
text row: `#` for a cell with any pixel that is not the background colour,
a space for an empty one, trailing spaces dropped.

That is the layout a person sees -- which rows hold text and how far each
reaches -- without reading the glyphs: a line drawn over another, a line
missing from the screen, or a prompt on the wrong row all change it. It
proves LAYOUT, not glyphs: a wrong character in a filled cell reads the
same. And it is valid only while the cursor does not blink -- a cursor cell
is inked or not depending on when the screendump lands -- which holds for
EDK2's GraphicsConsole: its cursor is drawn once and stays.

Usage: gop-cells.py SCREENDUMP.ppm COLS ROWS [CELL_W CELL_H]
"""

from __future__ import annotations

import sys
from pathlib import Path

GLYPH_W = 8
GLYPH_H = 19


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """Return (width, height, RGB bytes) of a binary PPM with maxval 255."""
    data = path.read_bytes()
    fields: list[bytes] = []
    pos = 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{path}: not a binary 8-bit PPM")
    width = int(fields[1])
    height = int(fields[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated pixel data")
    return width, height, pixels


def cell_rows(path: Path, cols: int, rows: int, cell_w: int, cell_h: int) -> list[str]:
    """The ink map of the text grid, one string per row."""
    width, height, pixels = read_ppm(path)
    x0 = (width - cols * cell_w) // 2
    y0 = (height - rows * cell_h) // 2
    if x0 < 0 or y0 < 0:
        raise ValueError(f"{path}: {width}x{height} is smaller than {cols}x{rows} cells")
    background = pixels[(y0 * width + x0) * 3:(y0 * width + x0) * 3 + 3]
    out: list[str] = []
    for row in range(rows):
        line: list[str] = []
        for col in range(cols):
            inked = False
            for y in range(y0 + row * cell_h, y0 + (row + 1) * cell_h):
                base = (y * width + x0 + col * cell_w) * 3
                cell = pixels[base:base + cell_w * 3]
                if any(cell[i:i + 3] != background for i in range(0, len(cell), 3)):
                    inked = True
                    break
            line.append("#" if inked else " ")
        out.append("".join(line).rstrip())
    return out


def main(argv: list[str]) -> int:
    """Print the ink map of the screendump argv names."""
    if len(argv) not in (4, 6):
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    cell_w = int(argv[4]) if len(argv) == 6 else GLYPH_W
    cell_h = int(argv[5]) if len(argv) == 6 else GLYPH_H
    for line in cell_rows(Path(argv[1]), int(argv[2]), int(argv[3]), cell_w, cell_h):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
