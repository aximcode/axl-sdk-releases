#!/usr/bin/env python3
"""
Synthesize tiny PNG fixtures for AxlPixmap unit tests.

Outputs:
  test-image-4x3.png       — 4x3 RGB PNG with a known color grid
                              (red/green/blue/white, black/gray/yellow/magenta,
                              cyan/orange/purple/brown).  92 bytes.

The corresponding .h file is generated via:
  xxd -i -n test_image_4x3_png test-image-4x3.png > test-image-4x3-png.h

Re-run this script when adding new fixture images.
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

DATA_DIR = Path(__file__).resolve().parent


def png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data))
    )


def make_4x3_png() -> bytes:
    # IHDR: 4w x 3h, 8-bit depth, color type 2 (RGB), no
    # compression/filter/interlace.
    ihdr = struct.pack(">IIBBBBB", 4, 3, 8, 2, 0, 0, 0)

    # 3 rows of (filter byte 0 + 4 RGB pixels = 12 bytes) = 39 raw bytes.
    pixels = bytes(
        [
            0, 255,   0,   0,    0, 255,   0,    0,   0, 255,  255, 255, 255,  # row 0
            0,   0,   0,   0,  128, 128, 128,  255, 255,   0,  255,   0, 255,  # row 1
            0,   0, 255, 255,  255, 165,   0,  128,   0, 128,  165,  42,  42,  # row 2
        ]
    )

    idat_data = zlib.compress(pixels)

    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", idat_data)
        + png_chunk(b"IEND", b"")
    )


def main() -> None:
    png = make_4x3_png()
    out_png = DATA_DIR / "test-image-4x3.png"
    out_png.write_bytes(png)
    print(f"Wrote {out_png.name}: {len(png)} bytes")


if __name__ == "__main__":
    main()
