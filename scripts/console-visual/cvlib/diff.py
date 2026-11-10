"""Cell-by-cell comparison of an oracle grid and a client grid, with every
difference classified ONCE against the case table's known-differences list.

Characters are compared first, then colours (both sides in EFI terms, the
terminal default resolved to the EFI default). The region compared is the
console's cols x rows; a client larger than the console must be blank
outside it, a smaller one is compared over the overlap and the cut reported.
"""
from __future__ import annotations

from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field

from cvlib.grid import Cell, Grid


@dataclass(frozen=True)
class Spot:
    """What a known-difference matcher sees: one differing cell in context."""

    x: int
    y: int
    cols: int
    oracle: Cell
    client: Cell
    oracle_row: str
    client_row: str
    console: tuple[int, int]  # the oracle's (the console's) cols x rows
    client_size: tuple[int, int]
    client_screen: str = ""  # the client's whole screen, as text
    oracle_screen: str = ""  # the oracle's whole screen, as text


@dataclass(frozen=True)
class Known:
    """A classified difference: its id, what it is, and whose it is."""

    kid: str
    owner: str  # "bug" (ours), "quirk" (TerminalDxe/oracle) or "design" (intended)
    what: str
    match: Callable[[Spot], bool]
    # The entry also accounts for a cursor that differs, in a step whose every
    # difference it (or another such entry) classified: a held client sits on
    # its notice, a line spliced into the oracle moves the oracle's cursor.
    owns_cursor: bool = False


@dataclass
class CellDiff:
    x: int
    y: int
    oracle: Cell
    client: Cell
    kind: str  # "char" or "colour"
    known: str = ""


@dataclass
class DiffResult:
    cols: int
    rows: int
    diffs: list[CellDiff] = field(default_factory=lambda: [])
    outside: int = 0  # non-blank client cells outside the console region
    cut: str = ""
    cursor_oracle: tuple[int, int] = (0, 0)
    cursor_client: tuple[int, int] = (0, 0)
    client_default_cells: int = 0
    closed: bool = False  # the client had exited: nothing was compared
    bugs: int = 0  # differences classified as an entry owned by "bug"
    cursor_known: bool = False  # a differing cursor is owned by a matched entry
    # Every difference is one the component intends (a known entry owned by
    # "design", e.g. a client deliberately held behind a notice).
    by_design: bool = False

    @property
    def char_diffs(self) -> int:
        return sum(1 for d in self.diffs if d.kind == "char")

    @property
    def colour_diffs(self) -> int:
        return sum(1 for d in self.diffs if d.kind == "colour")

    @property
    def cursor_differs(self) -> bool:
        return self.cursor_oracle != self.cursor_client

    @property
    def unknown(self) -> int:
        return (sum(1 for d in self.diffs if not d.known) + (1 if self.outside else 0)
                + (1 if self.cursor_differs and not self.cursor_known else 0))

    def fails(self) -> bool:
        """What fails a run: an unclassified difference (a cell, the cells
        outside the console, the cursor) or one classified as a bug. A step
        whose differences are all classified quirks is listed, not failed."""
        return not self.closed and (self.unknown > 0 or self.bugs > 0)

    def verdict(self) -> str:
        if self.closed:
            return "CLOSED"
        if self.by_design:
            return "BY-DESIGN"
        if not self.diffs and not self.outside and self.cursor_oracle == self.cursor_client:
            return "MATCH"
        if self.char_diffs or self.outside:
            return "DIFF-TEXT"
        if self.colour_diffs:
            return "DIFF-COLOUR"
        return "DIFF-CURSOR"


def compare(oracle: Grid, client: Grid, known: list[Known]) -> DiffResult:
    """``oracle`` is the oracle grid, ``client`` the client's (the names say which)."""
    cols, rows = oracle.cols, oracle.rows
    res = DiffResult(cols, rows, cursor_oracle=oracle.cursor, cursor_client=client.cursor,
                     client_default_cells=client.default_cells())
    if client.cols < cols or client.rows < rows:
        res.cut = (f"client {client.cols}x{client.rows} is smaller than the console "
                   f"{cols}x{rows}: compared over the overlap only")
    srows = ["".join(c.ch for c in r) for r in oracle.cells]
    trows = ["".join(c.ch for c in r) for r in client.cells]
    tscreen = "\n".join(trows)
    sscreen = "\n".join(srows)
    for y in range(min(rows, client.rows)):
        for x in range(min(cols, client.cols)):
            s, t = oracle.cells[y][x], client.cells[y][x]
            if s.ch != t.ch:
                kind = "char"
            elif s.ch.strip() and s.resolved() != t.resolved():
                kind = "colour"
            elif not s.ch.strip() and s.resolved()[1] != t.resolved()[1]:
                kind = "colour"  # a blank cell's background is visible
            else:
                continue
            d = CellDiff(x, y, s, t, kind)
            spot = Spot(x, y, cols, s, t, srows[y], trows[y], (cols, rows),
                        (client.cols, client.rows), tscreen, sscreen)
            for k in known:
                if k.match(spot):
                    d.known = k.kid
                    break
            res.diffs.append(d)
    for y in range(client.rows):
        for x in range(client.cols):
            if (y >= rows or x >= cols) and client.cells[y][x].ch.strip():
                res.outside += 1
    by_id = {k.kid: k for k in known}
    matched = [by_id[d.known] for d in res.diffs if d.known]
    res.bugs = sum(1 for k in matched if k.owner == "bug")
    res.cursor_known = (res.cursor_differs and len(matched) == len(res.diffs)
                        and any(k.owns_cursor for k in matched))
    # Out-of-region cells and the cursor are checked before the design
    # entries: neither is a cell a design matcher saw.
    res.by_design = (bool(res.diffs) and not res.outside and res.unknown == 0
                     and all(k.owner == "design" for k in matched))
    return res


def report(res: DiffResult, oracle: Grid, client: Grid, known_list: list[Known],
           names: tuple[str, str] = ("oracle", "client")) -> str:
    """diff.txt: counts, the rows that differ (side by side), colour pairs."""
    out: list[str] = [
        f"verdict: {res.verdict()}",
        f"S = {names[0]}, T = {names[1]}",
        f"console {res.cols}x{res.rows}; client {client.cols}x{client.rows}",
        f"char diffs: {res.char_diffs}  colour diffs: {res.colour_diffs}  "
        f"unclassified: {res.unknown}",
        f"cursor S={res.cursor_oracle} T={res.cursor_client}",
        f"T cells left at the terminal DEFAULT colour: {res.client_default_cells}"]
    if res.cut:
        out.append(res.cut)
    if res.outside:
        out.append(f"client has {res.outside} non-blank cells OUTSIDE the console region")
    known = Counter(d.known for d in res.diffs if d.known)
    for kid, n in sorted(known.items()):
        k = next(k for k in known_list if k.kid == kid)
        out.append(f"known {kid} ({k.owner}) x{n}: {k.what}")
    bad_rows = sorted({d.y for d in res.diffs})
    if bad_rows:
        out.append("")
        out.append("rows that differ (^ char, c colour):")
        slines = ["".join(c.ch for c in r) for r in oracle.cells]
        tlines = ["".join(c.ch for c in r[: res.cols]) for r in client.cells]
        for y in bad_rows:
            mark = [" "] * res.cols
            for d in res.diffs:
                if d.y == y:
                    mark[d.x] = "^" if d.kind == "char" else "c"
            out.append(f"{y + 1:3d} S|{slines[y].rstrip()}")
            out.append(f"    T|{tlines[y].rstrip() if y < len(tlines) else ''}")
            out.append(f"     |{''.join(mark).rstrip()}")
    pairs: Counter[str] = Counter()
    for d in res.diffs:
        if d.kind == "colour":
            sf, sb = d.oracle.resolved()
            tf, tb = d.client.resolved()
            pairs[f"{sf}/{sb} -> {tf}/{tb}"] += 1
    if pairs:
        out.append("")
        out.append("colour pairs (S fg/bg -> T fg/bg):")
        for p, n in pairs.most_common():
            out.append(f"  {n:5d}  {p}")
    return "\n".join(out) + "\n"


def selftest() -> list[str]:
    """The comparison must be able to FAIL: identical screens MATCH, and a
    one-character, a dim-vs-bright and a cursor difference are each seen.
    Also pins the colour normalisation the oracle relies on (1;33, 93 and
    38;5;11 are the same EFI yellow). Returns the failures."""
    import pyte  # pyright: ignore[reportMissingTypeStubs]

    from cvlib.grid import BceScreen, grid_from_screen

    def grid(vt: str) -> Grid:
        screen = BceScreen(20, 3)
        pyte.Stream(screen).feed(vt)
        return grid_from_screen(screen)

    base = "\x1b[1;33mFS0:\\>\x1b[0;37;40m ls"
    cases: list[tuple[str, str, str]] = [
        ("identical", base, "MATCH"),
        ("93 == 1;33", "\x1b[93mFS0:\\>\x1b[0;37;40m ls", "MATCH"),
        ("38;5;11 == 1;33", "\x1b[38;5;11mFS0:\\>\x1b[0;37;40m ls", "MATCH"),
        ("one char", "\x1b[1;33mFS0:\\>\x1b[0;37;40m lx", "DIFF-TEXT"),
        ("dim yellow", "\x1b[33mFS0:\\>\x1b[0;37;40m ls", "DIFF-COLOUR"),
        ("cursor", base + "\x1b[2;1H", "DIFF-CURSOR"),
    ]
    oracle = grid(base)
    fails: list[str] = []
    for name, vt, want in cases:
        got = compare(oracle, grid(vt), []).verdict()
        if got != want:
            fails.append(f"{name}: want {want}, got {got}")
    # BY-DESIGN only when EVERY difference is owned by "design".
    held = Known("held", "design", "", lambda p: p.y == 0)
    got = compare(oracle, grid(base + "\r\nheld"), [held]).verdict()
    if got != "DIFF-TEXT":
        fails.append(f"by-design with a row-1 difference outside it: got {got}")
    got = compare(oracle, grid("\x1b[1;33mFS0:\\>\x1b[0;37;40m lx"), [held]).verdict()
    if got != "BY-DESIGN":
        fails.append(f"by-design: want BY-DESIGN, got {got}")
    # What fails a run: an unclassified difference or one owned by "bug". A
    # step whose every difference is a classified quirk is listed, not failed.
    lx = grid("\x1b[1;33mFS0:\\>\x1b[0;37;40m lx")
    quirk = Known("q", "quirk", "", lambda p: p.y == 0)
    bug = Known("b", "bug", "", lambda p: p.y == 0)
    for name, res, want in (
            ("quirk-only", compare(oracle, lx, [quirk]), False),
            ("unclassified", compare(oracle, lx, []), True),
            ("bug-owned", compare(oracle, lx, [bug]), True),
            ("cursor-only", compare(oracle, grid(base + "\x1b[2;1H"), [quirk]), True),
            ("quirk + a cursor no entry owns", compare(oracle, grid(
                "\x1b[1;33mFS0:\\>\x1b[0;37;40m lx\x1b[2;1H"), [quirk]), True),
            ("match", compare(oracle, grid(base), []), False)):
        if res.fails() != want:
            fails.append(f"fails() for {name}: want {want}, got {res.fails()}")
    # BY-DESIGN only after the cells outside the console and the cursor: a
    # design entry accounts for the cursor only when it says it owns it.
    wide_screen = BceScreen(24, 3)
    pyte.Stream(wide_screen).feed("\x1b[1;33mFS0:\\>\x1b[0;37;40m lx\x1b[1;22Hzz\x1b[1;10H")
    got = compare(oracle, grid_from_screen(wide_screen), [held]).verdict()
    if got == "BY-DESIGN":
        fails.append("by-design with cells outside the console: got BY-DESIGN")
    moved = grid("\x1b[1;33mFS0:\\>\x1b[0;37;40m lx\x1b[2;1H")
    got = compare(oracle, moved, [held]).verdict()
    if got == "BY-DESIGN":
        fails.append("by-design with a cursor the entry does not own: got BY-DESIGN")
    held_cur = Known("held", "design", "", lambda p: p.y == 0, owns_cursor=True)
    res = compare(oracle, moved, [held_cur])
    if res.verdict() != "BY-DESIGN" or res.fails():
        fails.append(f"by-design owning the cursor: got {res.verdict()}, fails {res.fails()}")
    # Background-colour erase reaches cells never written (``cls 1``).
    bce = grid("\x1b[44m\x1b[2J").cells[2][19].bg
    if bce != "blue":
        fails.append(f"BCE: an ESC[2J under a blue pen left a cell {bce}")
    # ... and the line a scroll brings in: a terminal (and the GOP, which
    # fills it with the current background) paints it in the pen's colour.
    # Each case scrolls under a blue pen and names the row brought in.
    for name, vt, row in (("LF at the bottom", "\x1b[44m\x1b[3;1H\n", 2),
                          ("RI at the top", "\x1b[44m\x1b[1;1H\x1bM", 0),
                          ("IL", "\x1b[44m\x1b[2;1H\x1b[L", 1),
                          ("DL", "\x1b[44m\x1b[2;1H\x1b[M", 2)):
        got = {c.bg for c in grid(vt).cells[row]}
        if got != {"blue"}:
            fails.append(f"BCE: {name} under a blue pen left row {row + 1} {sorted(got)}")
    # A blank cell's foreground is invisible: only its background counts. A
    # glyph's foreground does.
    blank_fg = grid("\x1b[1;33mFS0:\\>\x1b[0;37;40m ls\x1b[31m     \x1b[37m")
    got = compare(grid(base + "     "), blank_fg, []).verdict()
    if got != "MATCH":
        fails.append(f"a blank cell's foreground: want MATCH, got {got}")
    got = compare(oracle, grid("\x1b[1;33mFS0:\\>\x1b[0;31;40m ls"), []).verdict()
    if got != "DIFF-COLOUR":
        fails.append(f"a glyph's foreground: want DIFF-COLOUR, got {got}")
    return fails
