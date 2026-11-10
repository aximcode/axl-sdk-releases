"""The cell grid every client and oracle is reduced to, in the EFI colour
model (16 foreground, 8 background colours; a terminal's "default" kept
distinct so it can be counted), plus the content-gated wait.

Every side is parsed by the same VT parser (pyte), so a difference between
two grids is a difference in the BYTES their producers emitted, not in two
parsers.
"""
from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass

import pyte  # pyright: ignore[reportMissingTypeStubs]
from pyte import graphics  # pyright: ignore[reportMissingTypeStubs]

# EFI's 16 foreground colours; backgrounds use the first 8.
EFI_NAMES: tuple[str, ...] = (
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "lightgray",
    "darkgray", "lightblue", "lightgreen", "lightcyan", "lightred", "lightmagenta",
    "yellow", "white")
_ANSI_TO_EFI: dict[str, str] = {
    "black": "black", "red": "red", "green": "green", "brown": "brown",
    "blue": "blue", "magenta": "magenta", "cyan": "cyan", "white": "lightgray",
    "brightblack": "darkgray", "brightred": "lightred", "brightgreen": "lightgreen",
    "brightbrown": "yellow", "brightblue": "lightblue", "brightmagenta": "lightmagenta",
    "brightcyan": "lightcyan", "brightwhite": "white"}
_BRIGHTEN: dict[str, str] = {
    "black": "darkgray", "red": "lightred", "green": "lightgreen", "brown": "yellow",
    "blue": "lightblue", "magenta": "lightmagenta", "cyan": "lightcyan",
    "lightgray": "white"}
# What a terminal's "default" colour is taken to be when comparing: the EFI
# console's own default. A cell left at the terminal default is still COUNTED
# (Grid.default_cells), because a light-themed terminal paints it differently.
DEFAULT_FG = "lightgray"
DEFAULT_BG = "black"


# pyte reports an indexed colour (38;5;N) as the hex of its xterm palette;
# indexes 0-15 ARE the 16 ANSI colours, which a terminal paints from its own
# 16-colour palette -- so they map back to names, not to pyte's RGB.
_INDEXED_16: dict[str, str] = dict(zip(
    graphics.FG_BG_256[:16],  # pyright: ignore[reportUnknownMemberType, reportUnknownArgumentType]
    ("black", "red", "green", "brown", "blue", "magenta", "cyan", "white",
     "brightblack", "brightred", "brightgreen", "brightbrown", "brightblue",
     "brightmagenta", "brightcyan", "brightwhite")))


def _efi_colour(name: str, bold: bool, is_fg: bool) -> str:
    if name == "default":
        return "default"
    name = _INDEXED_16.get(name, name)
    efi = _ANSI_TO_EFI.get(name)
    if efi is None:
        return f"#{name}"  # 256-colour / truecolour: pyte gives hex
    if is_fg and bold:
        return _BRIGHTEN.get(efi, efi)
    return efi


@dataclass(frozen=True)
class Cell:
    """One screen cell: the character and its colours in EFI terms."""

    ch: str
    fg: str
    bg: str

    def resolved(self) -> tuple[str, str]:
        fg = DEFAULT_FG if self.fg == "default" else self.fg
        bg = DEFAULT_BG if self.bg == "default" else self.bg
        return fg, bg


@dataclass
class Grid:
    """A screen: rows of cells and the cursor (0-based)."""

    cols: int
    rows: int
    cells: list[list[Cell]]
    cursor: tuple[int, int]

    def lines(self) -> list[str]:
        return ["".join(c.ch for c in row).rstrip() for row in self.cells]

    def text(self) -> str:
        return "\n".join(self.lines())

    def default_cells(self) -> int:
        return sum(1 for row in self.cells for c in row
                   if c.ch.strip() and (c.fg == "default" or c.bg == "default"))

    def cursor_line_prefix(self) -> str:
        x, y = self.cursor
        if not 0 <= y < self.rows:
            return ""
        return "".join(c.ch for c in self.cells[y][:x])


class BceScreen(pyte.Screen):
    """pyte.Screen with background-colour erase over EVERY cell.

    pyte's erase fills only the cells its sparse line dict already holds, so
    a cell never written keeps the default background after an ``ESC[2J``
    under a coloured pen -- while xterm, tmux and the firmware's own GOP
    (``cls 1`` paints the whole screen blue) erase all of them.
    """

    def erase_in_display(self, how: int = 0, *args: object, **kwargs: object) -> None:
        if how in (2, 3):
            self._fill(range(self.lines), range(self.columns))
        elif how == 0:
            self._fill(range(self.cursor.y + 1, self.lines), range(self.columns))
        elif how == 1:
            self._fill(range(self.cursor.y), range(self.columns))
        super().erase_in_display(how, *args, **kwargs)  # pyright: ignore[reportUnknownMemberType]

    def erase_in_line(self, how: int = 0, private: bool = False) -> None:
        if how == 0:
            self._fill((self.cursor.y,), range(self.cursor.x, self.columns))
        elif how == 1:
            self._fill((self.cursor.y,), range(self.cursor.x + 1))
        elif how == 2:
            self._fill((self.cursor.y,), range(self.columns))
        super().erase_in_line(how, private)

    # A scroll brings in a blank line, which pyte leaves at the default
    # colours; a terminal paints it with the current background, and so does
    # the GOP (GraphicsConsole fills the scrolled-in line with the current
    # attribute's background). Without this the serial oracle read black
    # where the firmware and ssh both show `cls 1`'s blue.
    def index(self) -> None:
        _, bottom = self.margins or pyte.screens.Margins(0, self.lines - 1)
        scrolls = self.cursor.y == bottom
        super().index()
        if scrolls:
            self._fill((bottom,), range(self.columns))

    def reverse_index(self) -> None:
        top, _ = self.margins or pyte.screens.Margins(0, self.lines - 1)
        scrolls = self.cursor.y == top
        super().reverse_index()
        if scrolls:
            self._fill((top,), range(self.columns))

    def insert_lines(self, count: int | None = None) -> None:
        count = count or 1
        top, bottom = self.margins or pyte.screens.Margins(0, self.lines - 1)
        y = self.cursor.y
        super().insert_lines(count)
        if top <= y <= bottom:
            self._fill(range(y, min(y + count, bottom + 1)), range(self.columns))

    def delete_lines(self, count: int | None = None) -> None:
        count = count or 1
        top, bottom = self.margins or pyte.screens.Margins(0, self.lines - 1)
        y = self.cursor.y
        super().delete_lines(count)
        if top <= y <= bottom:
            self._fill(range(max(bottom + 1 - count, y), bottom + 1), range(self.columns))

    def _fill(self, ys: range | tuple[int, ...], xs: range) -> None:
        for y in ys:
            line = self.buffer[y]
            for x in xs:
                line[x] = self.cursor.attrs


def grid_from_screen(screen: pyte.Screen) -> Grid:
    cells: list[list[Cell]] = []
    for y in range(screen.lines):
        line = screen.buffer[y]
        row: list[Cell] = []
        for x in range(screen.columns):
            c = line[x]
            fg = _efi_colour(c.fg, c.bold, True)
            bg = _efi_colour(c.bg, False, False)
            if c.reverse:
                fg, bg = bg, fg
            row.append(Cell(c.data or " ", fg, bg))
        cells.append(row)
    return Grid(screen.columns, screen.lines, cells, (screen.cursor.x, screen.cursor.y))


def wait_for(probe: Callable[[], bool], secs: float, poll: float = 0.05) -> float | None:
    """Poll until probe() is true; the seconds it took, or None on timeout."""
    t0 = time.monotonic()
    while True:
        if probe():
            return time.monotonic() - t0
        if time.monotonic() - t0 >= secs:
            return None
        time.sleep(poll)
