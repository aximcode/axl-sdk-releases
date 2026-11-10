"""Pictures for human review: a grid rendered as a terminal would show it
(EFI/VGA palette, monospace font), the picture-oracle-vs-client side-by-side,
and the 3-up oracle | client | red-overlay diff (the layout of agt's
visual-diff.py, at cell rather than pixel granularity, since the grids are
the oracle).
"""
from __future__ import annotations

from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from cvlib.diff import DiffResult
from cvlib.grid import Grid

PALETTE: dict[str, tuple[int, int, int]] = {
    "black": (0, 0, 0), "blue": (0, 0, 0xAA), "green": (0, 0xAA, 0), "cyan": (0, 0xAA, 0xAA),
    "red": (0xAA, 0, 0), "magenta": (0xAA, 0, 0xAA), "brown": (0xAA, 0x55, 0),
    "lightgray": (0xAA, 0xAA, 0xAA), "darkgray": (0x55, 0x55, 0x55),
    "lightblue": (0x55, 0x55, 0xFF), "lightgreen": (0x55, 0xFF, 0x55),
    "lightcyan": (0x55, 0xFF, 0xFF), "lightred": (0xFF, 0x55, 0x55),
    "lightmagenta": (0xFF, 0x55, 0xFF), "yellow": (0xFF, 0xFF, 0x55),
    "white": (0xFF, 0xFF, 0xFF)}
FONT_CANDIDATES = (
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf")
FONT_PX = 14
CELL_PAD = 1  # pixels above and below the font's line in a cell
# Images are kept under the 2000-px review limit.
MAX_W = 2000


@lru_cache(maxsize=1)
def _font() -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for f in FONT_CANDIDATES:
        if Path(f).is_file():
            return ImageFont.truetype(f, FONT_PX)
    return ImageFont.load_default()


def _cell_size() -> tuple[int, int]:
    """A cell holds the font's whole line -- ascent AND descent -- plus
    CELL_PAD above and below. Sized from a capital's box, it cut off
    everything under the baseline: an underscore drew in the row below,
    where the next row's paint (or the image's edge) erased it."""
    font = _font()
    if isinstance(font, ImageFont.FreeTypeFont):
        ascent, descent = font.getmetrics()
        height = ascent + descent
    else:
        height = int(font.getbbox("Mg_")[3])
    return int(round(font.getlength("M"))), height + 2 * CELL_PAD


def _rgb(name: str, fallback: str) -> tuple[int, int, int]:
    if name == "default":
        name = fallback
    if name.startswith("#") and len(name) == 7:
        return (int(name[1:3], 16), int(name[3:5], 16), int(name[5:7], 16))
    return PALETTE.get(name, (0xFF, 0, 0xFF))


def render_grid(grid: Grid, console: tuple[int, int] | None = None,
                marks: set[tuple[int, int]] | None = None) -> Image.Image:
    """The grid as pixels; ``console`` outlines the console region on a pane
    of another size; ``marks`` tints those cells red (the diff overlay)."""
    cw, ch = _cell_size()
    img = Image.new("RGB", (grid.cols * cw, grid.rows * ch), (0, 0, 0))
    draw = ImageDraw.Draw(img)
    font = _font()
    for y, row in enumerate(grid.cells):
        for x, c in enumerate(row):
            fg = _rgb(c.fg, "lightgray")
            bg = _rgb(c.bg, "black")
            box = (x * cw, y * ch, (x + 1) * cw - 1, (y + 1) * ch - 1)
            if bg != (0, 0, 0):
                draw.rectangle(box, fill=bg)
            if c.ch.strip():
                draw.text((x * cw, y * ch + CELL_PAD), c.ch, font=font, fill=fg)
    cx, cy = grid.cursor
    if 0 <= cy < grid.rows and 0 <= cx < grid.cols:
        draw.rectangle((cx * cw, (cy + 1) * ch - 3, (cx + 1) * cw - 1, (cy + 1) * ch - 1),
                       fill=(0xFF, 0xFF, 0xFF))
    if console and (console[0] != grid.cols or console[1] != grid.rows):
        draw.rectangle((0, 0, console[0] * cw - 1, console[1] * ch - 1),
                       outline=(0x00, 0xC0, 0xFF), width=2)
    if marks:
        red = Image.new("RGB", img.size, (0xFF, 0, 0))
        mask = Image.new("L", img.size, 0)
        md = ImageDraw.Draw(mask)
        for x, y in marks:
            md.rectangle((x * cw, y * ch, (x + 1) * cw - 1, (y + 1) * ch - 1), fill=110)
        img = Image.composite(red, img, mask)
    return img


def _label(img: Image.Image, text: str) -> Image.Image:
    bar = 20
    out = Image.new("RGB", (img.width, img.height + bar), (0x30, 0x30, 0x30))
    out.paste(img, (0, bar))
    ImageDraw.Draw(out).text((4, 3), text, font=_font(), fill=(0xFF, 0xFF, 0xFF))
    return out


def hstack(images: list[Image.Image], gap: int = 8) -> Image.Image:
    h = max(i.height for i in images)
    w = sum(i.width for i in images) + gap * (len(images) - 1)
    out = Image.new("RGB", (w, h), (0x30, 0x30, 0x30))
    x = 0
    for i in images:
        out.paste(i, (x, 0))
        x += i.width + gap
    if out.width > MAX_W:
        out = _resize(out, (MAX_W, int(out.height * MAX_W / out.width)))
    return out


def _fit_height(img: Image.Image, h: int) -> Image.Image:
    if img.height == h:
        return img
    return _resize(img, (int(img.width * h / img.height), h))


def _resize(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    # Pillow's stub types `size` with a numpy alias it cannot resolve here.
    return img.resize(size, Image.Resampling.LANCZOS)  # pyright: ignore[reportUnknownMemberType]


def write_pictures(stem: Path, picture: Path | None, picture_label: str, oracle: Grid,
                   oracle_tag: str, oracle_label: str, client: Grid, client_tag: str,
                   client_label: str, res: DiffResult) -> list[str]:
    """<tag>.png for the client and the grid oracle; side.png (picture
    oracle -- or the grid oracle when there is none -- next to the client);
    and, when the grids differ, diff3.png (oracle | client | red overlay).
    Returns the file names written."""
    console = (oracle.cols, oracle.rows)
    client_img = render_grid(client, console)
    oracle_img = render_grid(oracle)
    client_img.save(f"{stem}.{client_tag}.png")
    oracle_img.save(f"{stem}.{oracle_tag}.png")
    written = [f"{stem.name}.{client_tag}.png", f"{stem.name}.{oracle_tag}.png"]
    left, left_label = oracle_img, oracle_label
    if picture is not None and picture.is_file():
        with Image.open(picture) as im:
            left = im.convert("RGB")
        left_label = picture_label
    h = max(client_img.height, 400)
    side = hstack([_label(_fit_height(left, h), left_label),
                   _label(_fit_height(client_img, h), client_label)])
    side.save(f"{stem}.side.png")
    written.append(f"{stem.name}.side.png")
    if res.diffs or res.outside:
        marks = {(d.x, d.y) for d in res.diffs}
        three = hstack([_label(oracle_img, oracle_label),
                        _label(client_img, client_label),
                        _label(render_grid(client, console, marks), "differences (red)")])
        three.save(f"{stem}.diff3.png")
        written.append(f"{stem.name}.diff3.png")
    return written


def selftest() -> list[str]:
    """Every glyph is drawn inside its own cell: an underscore (below the
    baseline) on the first row and on the LAST row of a grid lights pixels
    within the cell's box, and none of a row's glyphs spill into the next.
    Returns the failures."""
    import pyte  # pyright: ignore[reportMissingTypeStubs]

    from cvlib.grid import BceScreen, grid_from_screen

    screen = BceScreen(4, 3)
    pyte.Stream(screen).feed("a_b\r\n\r\nx_y")
    grid = grid_from_screen(screen)
    grid.cursor = (3, 1)  # out of the way of the cells under test
    img = render_grid(grid)
    cw, ch = _cell_size()

    def bright(x: int, y: int) -> bool:
        px = img.getpixel((x, y))
        return isinstance(px, tuple) and sum(int(v) for v in px) > 150

    def lit(x0: int, y0: int, x1: int, y1: int) -> int:
        return sum(1 for x in range(x0, x1) for y in range(y0, y1) if bright(x, y))

    fails: list[str] = []
    for row in (0, 2):
        if lit(cw, row * ch, 2 * cw, (row + 1) * ch) == 0:
            fails.append(f"render: the underscore on row {row} drew nothing inside its cell")
    if lit(0, ch, 3 * cw, 2 * ch) != 0:
        fails.append("render: row 0's glyphs spilled into the blank row below")
    return fails
