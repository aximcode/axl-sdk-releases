"""Oracles: independent views of the SAME screen the client shows.

A grid oracle is compared with the client cell by cell; a picture oracle is
only rendered next to it for a human (or a model) to look at. Adding an
oracle: implement ``GridOracle`` or ``PictureOracle``.

- ``SerialPyteOracle``: the firmware's own serial VT stream (EDK2
  TerminalDxe) fed into pyte -- an encoder independent of AXL's.
- ``FirmwareShot``: the GOP framebuffer, via HMP ``screendump`` on the
  guest's monitor socket.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Protocol

import pyte  # pyright: ignore[reportMissingTypeStubs]
from PIL import Image

from cvlib.grid import BceScreen, Grid, grid_from_screen
from cvlib.guest import Guest, GuestError


class GridOracle(Protocol):
    tag: str  # file-name tag ("serial" -> NN-step.serial.txt)
    label: str

    def grid(self) -> Grid: ...


class PictureOracle(Protocol):
    tag: str
    label: str

    def shoot(self, png: Path, console: tuple[int, int]) -> None:
        """Write the picture; ``console`` is the console's cols x rows NOW."""
        ...


class SerialPyteOracle:
    """The serial log fed incrementally (from byte 0, so an attach replays
    the whole history) into a pyte screen of the console's size.

    The console's size is not fixed: a ``mode`` in the Shell reshapes it,
    and a serial terminal is never told (TerminalDxe's ``ESC[=3h`` names no
    size). ``geometry`` is a pattern over the serial BYTES whose groups are
    the new cols and rows -- a report the component writes to the serial
    port at the reshape, OUTSIDE the console (a control string a terminal
    discards, so nothing on the GOP or in a session changes) -- and the
    screen is resized at exactly that point of the stream and the report
    removed, so what follows is drawn at the new size and the report itself
    nowhere. In-band beats asking the guest: it is ordered with the output
    it describes, and asking (typing a command, opening a connection) would
    itself change the screen or the log.

    ``forbidden`` is what the component must never write through the
    console (the serial line carries everything ConOut does): seeing it
    fails the run, as a GuestError from ``grid``."""

    tag = "serial"
    label = "serial oracle (TerminalDxe -> pyte)"

    def __init__(self, guest: Guest, cols: int, rows: int,
                 geometry: re.Pattern[bytes] | None = None,
                 forbidden: re.Pattern[bytes] | None = None) -> None:
        self.log = guest.serial
        self.feeder = GeometryFeeder(cols, rows, geometry, forbidden)
        self.offset = 0

    def grid(self) -> Grid:
        with self.log.open("rb") as f:
            f.seek(self.offset)
            data = f.read()
        if data:
            self.offset += len(data)
            self.feeder.feed(data)
        if self.feeder.forbidden is not None:
            raise GuestError(f"the console carried {self.feeder.forbidden!r}: "
                             "a write the component must not make")
        return grid_from_screen(self.feeder.screen)


class GeometryFeeder:
    """A pyte screen fed bytes, resized wherever ``geometry`` matches and
    with the match removed (see SerialPyteOracle). Apart from the guest so it
    can be tested alone.

    A report split across two reads is still found and still removed: the
    tail of a read that may begin one (an unterminated ``ESC _``, or a lone
    trailing ESC) is held back until the next read completes it."""

    # A held tail this long is no report of ours: feed it as it is.
    HOLD_MAX = 128
    # The forbidden scan keeps this much of the previous read.
    CARRY = 256

    def __init__(self, cols: int, rows: int, geometry: re.Pattern[bytes] | None,
                 forbidden: re.Pattern[bytes] | None = None) -> None:
        self.screen = BceScreen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.geometry = geometry
        self.forbidden_re = forbidden
        self.forbidden: bytes | None = None
        self.held = b""
        self.carry = b""

    def feed(self, data: bytes) -> None:
        if self.forbidden_re is not None and self.forbidden is None:
            m = self.forbidden_re.search(self.carry + data)
            if m:
                self.forbidden = m.group(0)
            self.carry = (self.carry + data)[-self.CARRY:]
        if self.geometry is None:
            self.stream.feed(data)
            return
        window = self.held + data
        pos = 0
        for m in self.geometry.finditer(window):
            self.stream.feed(window[pos:m.start()])
            pos = m.end()
            cols, rows = int(m.group(1)), int(m.group(2))
            if (cols, rows) != (self.screen.columns, self.screen.lines):
                self.screen.resize(rows, cols)
        tail = window[pos:]
        hold = tail.rfind(b"\x1b_")
        if hold < 0 or len(tail) - hold > self.HOLD_MAX:
            hold = len(tail) - 1 if tail.endswith(b"\x1b") else len(tail)
        self.stream.feed(tail[:hold])
        self.held = tail[hold:]


class FirmwareShot:
    """The firmware's GOP screen, cropped to the text console."""

    tag = "firmware"
    label = "firmware GOP (screendump)"
    # EDK2 GraphicsConsole draws 8x19 glyphs and centres the text mode in
    # the GOP mode; the crop keeps the picture from being mostly border.
    GLYPH_W, GLYPH_H = 8, 19

    def __init__(self, guest: Guest, cols: int, rows: int) -> None:
        self.guest = guest

    def shoot(self, png: Path, console: tuple[int, int]) -> None:
        ppm = png.with_suffix(".ppm")
        ppm.unlink(missing_ok=True)
        out = self.guest.monitor(f"screendump {ppm}")
        if not ppm.is_file():
            raise GuestError(f"screendump wrote nothing: {out!r}")
        with Image.open(ppm) as im:
            img = im.convert("RGB")
        ppm.unlink()
        w, h = console[0] * self.GLYPH_W, console[1] * self.GLYPH_H
        if w <= img.width and h <= img.height:
            x0, y0 = (img.width - w) // 2, (img.height - h) // 2
            img = img.crop((x0, y0, x0 + w, y0 + h))
        img.save(png)
