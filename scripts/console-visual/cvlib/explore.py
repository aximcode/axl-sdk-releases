"""Exploring by hand: drive a LIVE session one invocation at a time.

Each invocation applies its actions (in command-line order) to a named
session of the kept guest, waits for the screen to settle, and prints the
client's grid, the diff against the serial oracle and the paths of the
step's files. Sessions persist BETWEEN invocations: each is a tmux session
on a private tmux server named after the guest, left open when the
invocation exits. Everything else -- clients, oracles, diff, pictures -- is
the suite runner's own code (cvlib.clients/oracles/diff/render).
"""
from __future__ import annotations

import argparse
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from cvlib.boot import boot_guest
from cvlib.cases import ClientContext, Suite
from cvlib.clients import Client, Tmux, TmuxClient
from cvlib.diff import compare, report
from cvlib.grid import Grid, wait_for
from cvlib.guest import Guest, GuestError
from cvlib.oracles import GridOracle
from cvlib.render import write_pictures

OK_VERDICTS = ("MATCH", "CLOSED", "BY-DESIGN")
SENDKEY_HOLD = 0.12  # QEMU's default key hold is 100 ms
SETTLE_MAX = 5.0  # the longest a settle waits for a moving screen
REPORT_LINES = 60  # a differing step's diff is cut to this many lines on stdout
ACTION_KINDS = ("type", "keys", "paste", "resize", "serial")

# QEMU monitor key tokens for the characters that are not a-z / 0-9.
_PLAIN = {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal",
          "[": "bracket_left", "]": "bracket_right", ";": "semicolon",
          "'": "apostrophe", "`": "grave_accent", "\\": "backslash", ",": "comma",
          ".": "dot", "/": "slash"}
_SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
            "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "bracket_left",
            "}": "bracket_right", ":": "semicolon", '"': "apostrophe", "~": "grave_accent",
            "|": "backslash", "<": "comma", ">": "dot", "?": "slash"}


def sendkey_tokens(text: str) -> list[str]:
    """``text`` as QEMU ``sendkey`` tokens (the guest's local keyboard)."""
    out: list[str] = []
    for ch in text:
        if ch.isascii() and ch.isalpha():
            out.append(ch.lower() if ch.islower() else f"shift-{ch.lower()}")
        elif ch.isascii() and ch.isdigit():
            out.append(ch)
        elif ch in _PLAIN:
            out.append(_PLAIN[ch])
        elif ch in _SHIFTED:
            out.append(f"shift-{_SHIFTED[ch]}")
        else:
            raise ValueError(f"--serial-type: no keyboard key for {ch!r}")
    return out


class _Collect(argparse.Action):
    """Append (kind, value) to ``namespace.actions``: the actions run in the
    order they were given, whichever option each came from."""

    def __call__(self, parser: argparse.ArgumentParser, namespace: argparse.Namespace,
                 values: object, option_string: str | None = None) -> None:
        acts: list[tuple[str, object]] = list(getattr(namespace, "actions", None) or [])
        acts.append((str(self.const), values))
        namespace.actions = acts


def add_arguments(p: argparse.ArgumentParser) -> None:
    g = p.add_argument_group("explore (console-visual.py explore ...)")
    g.add_argument("--start", action="store_true",
                   help="explore: build, boot and keep the guest, then open session a")
    g.add_argument("--session", default="a", help="explore: the session name (default a)")
    g.add_argument("--cols", type=int, help="explore: size of a NEW session (default: console)")
    g.add_argument("--rows", type=int)
    g.add_argument("--type", dest="_type", action=_Collect, const="type", metavar="TEXT",
                   help="explore: type TEXT literally (repeatable; use --type=TEXT for a "
                        "leading '-')")
    g.add_argument("--keys", dest="_keys", action=_Collect, const="keys", nargs="+",
                   metavar="NAME", help="explore: tmux key names (Enter Up C-c F2 ...)")
    g.add_argument("--paste", dest="_paste", action=_Collect, const="paste", metavar="FILE",
                   help="explore: paste FILE's contents as one paste")
    g.add_argument("--resize", dest="_resize", action=_Collect, const="resize",
                   metavar="CxR", help="explore: resize the terminal (a window-change)")
    g.add_argument("--serial-type", dest="_serial", action=_Collect, const="serial",
                   metavar="TEXT",
                   help="explore: type TEXT on the guest's LOCAL keyboard (QEMU sendkey; "
                        "the console side, not ssh)")
    g.add_argument("--wait-for", metavar="TEXT",
                   help="explore: wait until a row of the ssh pane contains TEXT")
    g.add_argument("--settle", type=int, default=300, metavar="MS",
                   help="explore: wait until the screens are unchanged this long (300)")
    g.add_argument("--timeout", type=float, default=10.0,
                   help="explore: bound on --wait-for, seconds (10)")
    g.add_argument("--shot", action="store_true",
                   help="explore: write firmware PNG, ssh PNG and side-by-side")
    g.add_argument("--sgr", action="store_true",
                   help="explore: print the pane with its SGR escapes (shown as \\e)")
    g.add_argument("--new-session", action="store_true",
                   help="explore: close the session if open and open a fresh one")
    g.add_argument("--close-session", action="store_true",
                   help="explore: close the session after the actions")


# --- sessions --------------------------------------------------------------


@dataclass
class Sessions:
    """The persistent sessions of one guest: open, reattach, close."""

    tmux: Tmux
    make_client: Callable[[str, tuple[int, int]], Client]  # tmux session name, size
    ready: Callable[[Grid], bool]  # a fresh session is usable (a prompt, a notice)
    default_size: tuple[int, int]
    ready_secs: float = 20.0

    @staticmethod
    def tname(name: str) -> str:
        return f"x-{name}"

    def exists(self, name: str) -> bool:
        return self.tname(name) in self.tmux.sessions()

    def get(self, name: str, new: bool, size: tuple[int, int] | None) -> tuple[Client, bool]:
        """The client of session ``name`` and whether it was just opened."""
        if new:
            self.close(name)
        if self.exists(name):
            out = self.tmux.run("display", "-p", "-t", self.tname(name),
                                "#{window_width}x#{window_height}").strip()
            cols, rows = (int(v) for v in out.split("x"))
            return self.make_client(self.tname(name), (cols, rows)), False
        client = self.make_client(self.tname(name), size or self.default_size)
        client.open()
        if wait_for(lambda: self.ready(client.grid()) or client.dead(), self.ready_secs) is None \
                or client.dead():
            text = client.grid().text()
            client.close()
            raise GuestError(f"session {name}: never became ready\n{text}")
        return client, True

    def close(self, name: str) -> bool:
        if not self.exists(name):
            return False
        self.tmux.run("kill-session", "-t", self.tname(name), check=False)
        return True


def apply_action(client: Client, guest: Guest | None, kind: str, value: object) -> None:
    if kind == "type":
        client.act("type", str(value))
    elif kind == "keys":
        client.act("keys", tuple(str(v) for v in value))  # pyright: ignore[reportGeneralTypeIssues]
    elif kind == "paste":
        client.act("paste", Path(str(value)).read_text())
    elif kind == "resize":
        client.act("resize", str(value))
    elif kind == "serial":
        if guest is None:
            raise GuestError("--serial-type needs the guest")
        for tok in sendkey_tokens(str(value)):
            guest.monitor(f"sendkey {tok}")
            # sendkey returns at once and the key is held 100 ms: without
            # the wait, the NEXT action (an ssh Enter) overtakes the typing.
            time.sleep(SENDKEY_HOLD)
    else:
        raise ValueError(f"unknown explore action {kind!r}")


def settle(grids: Callable[[], list[object]], quiet: float, limit: float) -> bool:
    """True once ``grids()`` is unchanged for ``quiet`` seconds, within ``limit``."""
    last, since = grids(), time.monotonic()
    deadline = since + limit
    while time.monotonic() < deadline:
        time.sleep(0.05)
        cur = grids()
        if cur != last:
            last, since = cur, time.monotonic()
        elif time.monotonic() - since >= quiet:
            return True
    return False


def pane_text(client: Client, sgr: bool) -> str:
    """The client's pane, rows numbered, trailing blanks trimmed; the SGR
    form (``\\e`` for ESC) when ``sgr``."""
    if sgr and isinstance(client, TmuxClient):
        raw = client.capture(sgr=True).replace("\x1b", "\\e")
    else:
        raw = client.grid().text()
    rows = [r.rstrip() for r in raw.split("\n")]
    while rows and not rows[-1]:
        rows.pop()
    return "\n".join(f"{i + 1:3d}|{r}" for i, r in enumerate(rows))


# --- the command -----------------------------------------------------------


def tmux_for(suite: Suite, arch: str, workdir: Path) -> Tmux:
    return Tmux(f"cv-explore-{suite.name}-{arch}", workdir / "explore-tmux.conf")


def stop_tmux(suite: Suite, arch: str, workdir: Path) -> None:
    """Close every explore session and remove the tmux server's socket."""
    if workdir.is_dir():
        tmux_for(suite, arch, workdir).kill_server()


def _next_seq(outdir: Path) -> int:
    outdir.mkdir(parents=True, exist_ok=True)
    seq = outdir / ".seq"
    n = int(seq.read_text()) + 1 if seq.is_file() else 1
    seq.write_text(str(n))
    return n


def explore_main(args: argparse.Namespace, suite: Suite, state_file: Path,
                 outdir: Path) -> int:
    if args.start:
        guest, _ = boot_guest(suite, args.arch, state_file, args.no_build, True)
    else:
        guest = Guest.attach(state_file, suite.name, args.arch)
    boot_cols, boot_rows = suite.console_size(guest)
    # The oracle replays the serial log from byte 0, so it ends at the
    # console's size NOW -- a `mode` since the boot included.
    oracle = suite.grid_oracle(guest, boot_cols, boot_rows)
    now = oracle.grid()
    cols, rows = now.cols, now.rows
    tmux = tmux_for(suite, args.arch, state_file.parent)
    workdir = state_file.parent

    def make_client(tname: str, size: tuple[int, int]) -> Client:
        return suite.client(ClientContext(tmux, guest, tname, size,
                                          workdir / f"{tname}.raw"))

    def ready(g: Grid) -> bool:
        # A prompt, a held notice, or -- the Shell line may hold typed text
        # from an earlier session -- any painted text at all.
        return (bool(suite.prompt.match(g.cursor_line_prefix()))
                or (suite.held is not None and bool(suite.held.search(g.text())))
                or bool(g.text().strip()))

    sessions = Sessions(tmux, make_client, ready, (cols, rows))
    size = (args.cols or cols, args.rows or rows)
    name: str = args.session
    client, opened = sessions.get(name, args.new_session or args.start, size)
    if opened:
        print(f"opened session {name} at {size[0]}x{size[1]}", flush=True)
    actions: list[tuple[str, object]] = list(getattr(args, "actions", None) or [])
    if opened and not (suite.held and suite.held.search(client.grid().text())):
        _start_screen(suite, client, oracle)

    def snap() -> list[object]:
        out: list[object] = [oracle.grid().text()]
        if not client.dead():
            g = client.grid()
            out += [g.text(), g.cursor]
        return out

    for kind, value in actions:
        apply_action(client, guest, kind, value)
    met = True
    if args.wait_for:
        want = str(args.wait_for)
        met = wait_for(lambda: any(want in ln for ln in client.grid().lines()),
                       args.timeout) is not None
    settled = settle(snap, args.settle / 1000, SETTLE_MAX)
    n = _next_seq(outdir)
    dead = client.dead()
    return _print_step(args, suite, guest, outdir, n, name, client, oracle.grid(), dead,
                       met, settled, sessions)


def _start_screen(suite: Suite, client: Client, oracle: GridOracle) -> None:
    """Bring a fresh session to the suite's known screen (``cls``): the
    serial oracle replays the whole boot, so without it the two differ by
    history rather than by anything the user did."""
    # The Shell's line is shared with every other console: drop whatever an
    # earlier session left typed on it.
    client.act("keys", ("Escape",))
    for a in suite.start:
        if a.kind == "cmd":
            client.act("type", str(a.arg))
            client.act("keys", ("Enter",))
        else:
            client.act(a.kind, a.arg)

    def home() -> bool:
        g = client.grid()
        return g.cursor[1] == 0 and bool(suite.prompt.match(g.cursor_line_prefix()))

    wait_for(lambda: home() and oracle.grid().cursor[1] == 0, 10)


def _print_step(args: argparse.Namespace, suite: Suite, guest: Guest, outdir: Path, n: int,
                name: str, client: Client, og: Grid, dead: bool, met: bool, settled: bool,
                sessions: Sessions) -> int:
    console = (og.cols, og.rows)
    outdir.mkdir(parents=True, exist_ok=True)
    stem = outdir / f"{time.strftime('%Y%m%d-%H%M%S')}-{n}"
    cg = client.grid()
    files = client.save(stem)
    Path(f"{stem}.serial.txt").write_text(og.text() + "\n")
    files.append(f"{stem.name}.serial.txt")
    res = compare(og, cg, suite.known)
    body = report(res, og, cg, suite.known, ("serial oracle", client.describe()))
    verdict = "CLOSED" if dead else res.verdict()
    if dead:
        body = "the client has EXITED (session closed): not compared\n"
    Path(f"{stem}.diff.txt").write_text(body)
    files.append(f"{stem.name}.diff.txt")
    if args.shot and not dead:
        if suite.picture_oracle is not None:
            pic = suite.picture_oracle(guest, *console)
            shot = Path(f"{stem}.{pic.tag}.png")
            pic.shoot(shot, console)
            files.append(shot.name)
            files += write_pictures(stem, shot, pic.label, og, "serial",
                                    "serial oracle (TerminalDxe -> pyte)", cg, client.tag,
                                    client.describe(), res)
    print(f"== session {name}: {client.describe()}, console {console[0]}x{console[1]}; "
          f"verdict {verdict} ==")
    if not met:
        print(f"!! --wait-for {args.wait_for!r} never appeared in {args.timeout:.0f}s")
    if not settled:
        print(f"!! still changing after {SETTLE_MAX:.0f}s (captured anyway)")
    print(pane_text(client, args.sgr))
    print(f"cursor ssh={cg.cursor} serial={og.cursor}")
    if verdict in OK_VERDICTS:
        print(f"diff: {verdict}; {res.char_diffs} char, {res.colour_diffs} colour, "
              f"{res.unknown} unclassified")
    else:
        print(f"diff: {verdict}; {res.char_diffs} char, {res.colour_diffs} colour, "
              f"{res.unknown} unclassified, {res.bugs} bug-classified")
        lines = body.splitlines()
        print("\n".join(lines[:REPORT_LINES]))
        if len(lines) > REPORT_LINES:
            print(f"... {len(lines) - REPORT_LINES} more lines in {stem.name}.diff.txt")
    print("files: " + " ".join(str(outdir / f) for f in files))
    if args.close_session:
        sessions.close(name)
        print(f"closed session {name}")
    return 0


# --- selftest --------------------------------------------------------------


def selftest() -> list[str]:
    """Two consecutive 'invocations' (separate Tmux and Sessions objects)
    share a session: what the first types, the second sees; a --new-session
    does not. Also pins the action order and the sendkey map."""
    import tempfile

    fails: list[str] = []
    ns = argparse.Namespace()
    p = argparse.ArgumentParser()
    add_arguments(p)
    ns = p.parse_args(["--type=a", "--keys", "Enter", "Up", "--serial-type", "b", "--type=-c"])
    got = [(k, v) for k, v in ns.actions]
    want: list[tuple[str, object]] = [("type", "a"), ("keys", ["Enter", "Up"]),
                                      ("serial", "b"), ("type", "-c")]
    if got != want:
        fails.append(f"explore: actions out of order: {got}")
    if sendkey_tokens("aZ1 !\n") != ["a", "shift-z", "1", "spc", "shift-1", "ret"]:
        fails.append("explore: sendkey_tokens map is wrong")
    with tempfile.TemporaryDirectory() as d:
        wd = Path(d)
        name = f"cv-explore-selftest-{time.monotonic_ns()}"

        def invocation() -> Sessions:
            tmux = Tmux(name, wd / "tmux.conf")

            def mk(tname: str, size: tuple[int, int]) -> Client:
                return TmuxClient(tmux, tname, ["cat"], size[0], size[1], wd / f"{tname}.raw",
                                  tag="ssh")
            return Sessions(tmux, mk, lambda g: True, (40, 5), 5.0)

        try:
            s1 = invocation()
            c1, opened = s1.get("t", False, None)
            c1.act("type", "persisted-text")
            if not opened:
                fails.append("explore: first get did not open the session")
            wait_for(lambda: "persisted-text" in c1.grid().text(), 5)
            c2, opened2 = invocation().get("t", False, None)
            if opened2 or "persisted-text" not in c2.grid().text():
                fails.append("explore: the second invocation did not find the first's text")
            c3, opened3 = invocation().get("t", True, (30, 4))
            if not opened3 or "persisted-text" in c3.grid().text() or (c3.cols, c3.rows) != (30, 4):
                fails.append("explore: --new-session did not give a fresh 30x4 session")
        finally:
            Tmux(name, wd / "tmux.conf").kill_server()
    return fails
