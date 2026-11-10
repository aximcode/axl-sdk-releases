"""The sshd suite: the live UEFI Shell served over SSH by tools/sshd.c.

Client under test: a real OpenSSH ``ssh -tt`` in a tmux pane (TERM
xterm-256color, as a typical user's terminal). Grid oracle: the firmware's
own serial terminal (EDK2 TerminalDxe renders the same console under
sshd's passthrough). Picture oracle: the GOP framebuffer.

The boot is ssh-demo.sh's recipe (connect, DHCP, then ``sshd -p 22 -x
<slirp gateway>``) run through run-qemu.sh, as everything here is.
"""
from __future__ import annotations

import re
import subprocess
import unicodedata
from pathlib import Path

from cvlib.cases import PROMPT, Act, ClientContext, Cond, Scenario, Step, Suite, cmd, keys, typed
from cvlib.clients import Client, TmuxClient
from cvlib.diff import Known, Spot
from cvlib.guest import PROJECT_DIR, BootSpec, Guest, GuestError, build_prefix
from cvlib.oracles import FirmwareShot, SerialPyteOracle

GUEST_PORT = 22
# The slirp gateway: every host connection reaches the guest from it.
HOST_AS_SEEN = "10.0.2.2/32"
SERVING_RE = re.compile(rb"sshd: serving the UEFI Shell on .+ port \d+ \(console (\d+)x(\d+)\)")
# sshd's report when a `mode` reshapes the console (`--report-geometry`,
# tools/sshd-svc.c): an APC string written to the serial port itself, never
# to ConOut. The serial oracle resizes at it and removes it
# (SerialPyteOracle's ``geometry``).
GEOMETRY_RE = re.compile(rb"\x1b_axl-console-size=(\d+)x(\d+)\x1b\\")
# What sshd once LOGGED at a reshape: through ConOut, onto the GOP, the
# serial line and every session. The oracle fails the run if it reappears.
CONSOLE_GEOMETRY_RE = re.compile(rb"the console is now \d+x\d+")
PROMPT_RE = re.compile(r"^(?:FS\d+:\\.*|Shell)> $")

# Fixtures staged next to sshd.efi: long.txt scrolls any console; loop.nsh
# runs until a Ctrl-C; edit.txt is copied (never edited) by the edit case.
FIXTURES: dict[str, str] = {
    "long.txt": "".join(f"long line {i:03d} the quick brown fox jumps over the lazy dog\r\n"
                        for i in range(1, 121)),
    "loop.nsh": "for %a run (1 300)\r\n  echo loop-%a\r\n  stall 200000\r\nendfor\r\n",
    "edit.txt": "".join(f"edit line {i:02d}\r\n" for i in range(1, 61)),
    # Lines wider than any console here (150 columns): the Shell wraps them.
    "wide.txt": "".join(f"wide {i:02d} " + "".join(chr(ord("a") + (i + k) % 26)
                                                    for k in range(140)) + "\r\n"
                        for i in range(1, 13)),
}
LONG_PASTE = "echo " + "-".join(f"p{i:03d}" for i in range(60))
# An echo longer than two console rows: it wraps at the console's width.
LONG_ECHO = "echo " + " ".join(f"w{i:03d}" for i in range(50))
# ssh(1)'s escape: Enter, then ~. closes the session from the client side.
DETACH_B = (keys("Enter", on="b"), typed("~.", on="b"))


def build(arch: str) -> None:
    res = subprocess.run([str(PROJECT_DIR / "scripts" / "axl-build.sh"), f"ARCH={arch}",
                          "sshd"], capture_output=True, text=True)
    if res.returncode != 0:
        raise GuestError(f"axl-build.sh sshd failed:\n{res.stdout[-2000:]}{res.stderr[-2000:]}")


def boot(arch: str, workdir: Path) -> BootSpec:
    workdir.mkdir(parents=True, exist_ok=True)
    key = workdir / "key"
    for p in (key, workdir / "key.pub"):
        p.unlink(missing_ok=True)
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "console-visual",
                    "-f", str(key)], check=True, stdin=subprocess.DEVNULL)
    auth = workdir / "authorized_keys"
    auth.write_text((workdir / "key.pub").read_text())
    extras = [auth]
    for name, body in FIXTURES.items():
        (workdir / name).write_text(body)
        extras.append(workdir / name)
    return BootSpec(
        efi=build_prefix(arch) / "tools" / "sshd.efi",
        nsh_lines=["@echo -off", "fs0:", "cd \\", "connect -r", "stall 1000000",
                   "ifconfig -s eth0 dhcp", "stall 3000000",
                   f"sshd -p {GUEST_PORT} -x {HOST_AS_SEEN} --report-geometry"],
        extras=extras, hostfwd=[GUEST_PORT], ready=SERVING_RE,
        ready_secs=240 if arch == "aa64" else 120)


def console_size(guest: Guest) -> tuple[int, int]:
    m = SERVING_RE.search(guest.serial_bytes())
    if not m:
        raise GuestError("sshd's serving line carries no console size")
    return int(m.group(1)), int(m.group(2))


def client(ctx: ClientContext) -> Client:
    st = ctx.guest.state
    command = ["ssh", "-tt", "-p", str(st.ports[str(GUEST_PORT)]),
               "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
               "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20",
               "-i", str(Path(st.workdir) / "key"), "user@localhost"]
    return TmuxClient(ctx.tmux, ctx.name, command, ctx.size[0], ctx.size[1], ctx.raw_log,
                      tag="ssh")


def mark(marker: str) -> tuple[tuple[Act, ...], tuple[Cond, ...]]:
    return (cmd(f"echo {marker}"),), (Cond("line", marker), PROMPT)


# sshd's notice to a terminal smaller than the console (tools/sshd-bind.c).
HELD_RE = re.compile(r"\[sshd\] this terminal is \d+x\d+; the console is \d+x\d+\.")

# The level tag, not the timestamp: typed input can overwrite the stamp.
LOG_LINE_RE = re.compile(r"\[(?:INFO|DEBUG|WARN|ERROR)\]  ")

def q1_lastcol(p: Spot) -> bool:
    """TerminalDxe's stale last-column cell, in the one shape it has been
    seen in (paste, info/echo-long, info/error; the GOP agrees with ssh): a
    line the Shell wraps, identical on both sides but for its last cell, where
    the oracle repeats the cell before it and the client has a character; and
    the next row continues the wrapped text identically on both sides. A blank,
    a different character, or a continuation that differs is a margin
    regression of ours (the encoder writes CR LF at that cell) -- unclassified."""
    x = p.x
    if x != p.cols - 1 or x == 0 or not p.client.ch.strip():
        return False
    if p.oracle_row[:x] != p.client_row[:x] or p.oracle.ch != p.oracle_row[x - 1]:
        return False
    orows, crows = p.oracle_screen.split("\n"), p.client_screen.split("\n")
    if p.y + 1 >= min(len(orows), len(crows)):
        return False
    nxt_o, nxt_c = orows[p.y + 1][:x], crows[p.y + 1][:x]
    return bool(nxt_c[:1].strip()) and nxt_o == nxt_c


def _wide(ch: str) -> bool:
    return unicodedata.east_asian_width(ch) in ("W", "F")


def narrowed(row: str) -> str:
    """``row`` as sshd sends it: each wide codepoint one '?' cell. In a pyte
    row a wide glyph is followed by its continuation cell, read as a space --
    unless the firmware's next character was written over it."""
    out: list[str] = []
    i = 0
    while i < len(row):
        if _wide(row[i]):
            out.append("?")
            i += 2 if i + 1 < len(row) and row[i + 1] == " " else 1
        else:
            out.append(row[i])
            i += 1
    return "".join(out)


def q3_wide(p: Spot) -> bool:
    """TerminalDxe's VT-UTF8 stream sends a CJK codepoint raw, so the serial
    terminal draws it in two cells where the console counts one; sshd sends
    one '?' per codepoint and keeps the console's grid. The whole row must be
    exactly the oracle's with each wide glyph narrowed: anything else in it is
    unclassified."""
    return (any(_wide(c) for c in p.oracle_row)
            and narrowed(p.oracle_row).rstrip() == p.client_row.rstrip())


# Every entry is a CLASSIFICATION, not a waiver: a step whose only
# differences are known still reports DIFF-*, and diff.txt names each id.
# Bugs carry their report number, so a fix shows up as the entry going
# unmatched. The GOP screendump is the tie-breaker for which side is right.
KNOWN: list[Known] = [
    Known("Q1-terminaldxe-lastcol", "quirk",
          "TerminalDxe's cell in the LAST column of a line the Shell wraps keeps a stale "
          "character, the one before it repeated (the GOP agrees with ssh)",
          q1_lastcol),
    Known("Q2-serial-only-log", "quirk",
          "an AxlLog line (e.g. sshd's 'Accepted publickey') is written to the serial "
          "port only -- not to the console ssh and the GOP show -- so it splices into "
          "the serial user's screen",
          lambda p: bool(LOG_LINE_RE.search(p.oracle_row))
          and not LOG_LINE_RE.search(p.client_row), owns_cursor=True),
    Known("Q3-terminaldxe-wide", "quirk",
          "TerminalDxe sends a wide (CJK) codepoint raw, so the serial terminal draws it "
          "in two cells; the console counts one, and sshd sends one '?' per codepoint "
          "(the GOP: one cell each when echoed, none when the Shell redraws the line -- "
          "its font has no CJK glyphs)",
          q3_wide),
    Known("D1-held-smaller", "design",
          "a terminal SMALLER than the console is held: it shows sshd's notice (both "
          "sizes, enlarge to continue) instead of the console, until a window-change "
          "makes it large enough",
          lambda p: HELD_RE.search(p.client_screen) is not None
          and (p.client_size[0] < p.console[0] or p.client_size[1] < p.console[1]),
          owns_cursor=True),
]


# Terminal sizes are chosen against BOTH consoles (x64 100x31, aa64 80x50):
# 132x60 is larger than either, 80x24 smaller than either.
CASES: tuple[Scenario, ...] = (
    Scenario("basic", "ver, ls, map -r on a console-sized pane", (
        Step("ver", (cmd("ver"),), (Cond("has", "UEFI Interactive Shell"), PROMPT)),
        Step("ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        Step("map-r", (cmd("map -r"),), (Cond("has", "Mapping table"), PROMPT)),
    )),
    Scenario("colours", "ls directory colours and help's highlighted text", (
        Step("ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        Step("help-ls", (cmd("help ls"),), (Cond("has", "EXAMPLES"), PROMPT)),
        Step("ls-dir-efi", (cmd("ls EFI"),), (Cond("has", "Dir(s)"), PROMPT)),
    )),
    Scenario("scroll", "long scrolling output: dh, help -b paging, type of a long file", (
        Step("dh", (cmd("dh"),), (PROMPT,), timeout=30),
        Step("type-long", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
        Step("help-b-page1", (cmd("help -b"),), (Cond("has", "Press ENTER to continue"),)),
        Step("help-b-page2", (keys("Enter"),), (Cond("has", "Press ENTER to continue"),)),
        Step("help-b-quit", (typed("q"),), (PROMPT,)),
    )),
    Scenario("edit", "the Shell's full-screen editor over ssh", (
        # A fresh copy each run: the scenario edits and saves it.
        Step("copy", (cmd("cp -q edit.txt sv-edit.txt"),), (PROMPT,)),
        Step("open", (cmd("edit sv-edit.txt"),), (Cond("has", "edit line 01"),
                                               Cond("gone", "Shell>"))),
        Step("type-hello", (typed("HELLO "),), (Cond("has", "HELLO edit line 01"),)),
        Step("down3-right2", (keys("Down", "Down", "Down", "Right", "Right"), typed("X")),
             (Cond("has", "edit linXe 04"),)),
        Step("pgdn", (keys("PageDown"),), (Cond("gone", "HELLO"),)),
        Step("pgup", (keys("PageUp"),), (Cond("has", "HELLO edit line 01"),)),
        Step("end-type", (keys("End"), typed("<END")), (Cond("has", "<END"),)),
        Step("home-type", (keys("Home"), typed("H>")), (Cond("has", "H>"),)),
        Step("save", (keys("F2"),), (Cond("has", "File to Save"),)),
        Step("save-confirm", (keys("Enter"),), (Cond("has", "Lines Written"),)),
        Step("quit", (keys("F3"),), (PROMPT,)),
        Step("type-saved", (cmd("type sv-edit.txt"),), (Cond("has", "edit line 60"), PROMPT)),
    )),
    Scenario("ctrlc", "Ctrl-C over ssh breaks a looping script", (
        Step("loop-running", (cmd("loop.nsh"),), (Cond("line", "loop-3"),), settle=False),
        Step("ctrl-c", (keys("C-c"),), (PROMPT,)),
    )),
    # A control key's notify fires when it is PUSHED; the Shell's Ctrl-S
    # halts output and the next key READ resumes it. At the end of one
    # write, ^S must act after the text before it: the line keeps every
    # character, the halt lands on the next output (Enter's), and the key
    # that resumes it is consumed, as at the local keyboard.
    Scenario("ctrl-s", "Ctrl-S at the end of a burst, and Ctrl-S halting a running loop", (
        Step("burst", (Act("paste", "echo abcdef\x13"),),
             (Cond("cursorline", "> echo abcdef"),)),
        Step("enter-halted", (keys("Enter"),), (Cond("cursorline", "> echo abcdef"),),
             still=1.0),
        Step("key-resumes", (typed("x"),), (Cond("line", "abcdef"), PROMPT)),
        Step("loop-running", (cmd("loop.nsh"),), (Cond("line", "loop-3"),), settle=False),
        Step("ctrl-s-halts", (keys("C-s"),), (Cond("has", "loop-"),), still=1.5),
        Step("key-resumes-loop", (typed("x"),), (Cond("has", "loop-"),), settle=False),
        Step("ctrl-c", (keys("C-c"),), (PROMPT,)),
    )),
    Scenario("latency", "per-key echo time, ssh vs the firmware's serial terminal", (
        Step("type-20", (Act("keylat", "echo abcdefghijklmnop"),),
             (Cond("has", "echo abcdefghijklmnop"),)),
        Step("run", (keys("Enter"),), (Cond("line", "abcdefghijklmnop"), PROMPT)),
    )),
    Scenario("paste", "a long command line pasted at once", (
        Step("paste-long", (Act("paste", LONG_PASTE + "\r"),),
             (Cond("joined", LONG_PASTE[5:]), PROMPT)),
    )),
    Scenario("multi", "two sessions; a late joiner is repainted with the current screen", (
        Step("a-ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        # The repaint is the client's alone: the console itself does not change.
        Step("b-joins", (Act("open", "b"),), (Cond("has", "Dir(s)", client_only=True),
                                              Cond("prompt", client_only=True))),
        Step("b-types-ver", (cmd("ver", on="b"),), (Cond("has", "UEFI Interactive Shell"),
                                                    PROMPT)),
        Step("b-detach", DETACH_B, (Cond("dead", "b", client_only=True),)),
        Step("a-after", (cmd("echo still-here"),), (Cond("line", "still-here"), PROMPT)),
    ), sessions={"a": None, "b": None}),
    Scenario("pane-large", "an ssh terminal LARGER than the console (132x60)", (
        Step("ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        Step("type-long", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
        Step("type-wide", (cmd("type wide.txt"),), (Cond("has", "wide 12"), PROMPT)),
        Step("edit-open", (cmd("edit edit.txt"),), (Cond("has", "edit line 01"),
                                                    Cond("gone", "Shell>"))),
        Step("edit-pgdn", (keys("PageDown"),), (Cond("gone", "edit line 01"),)),
        Step("edit-quit", (keys("F3"),), (PROMPT,)),
        Step("after-edit", (cmd("ver"),), (Cond("has", "UEFI Interactive Shell"), PROMPT)),
        # Shrunk below the console: held behind the notice ...
        Step("shrink", (Act("resize", "80x24"),), (Cond("has", "this terminal is 80x24",
                                                        client_only=True),)),
        Step("held-ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        # ... and back: repainted, confined to the console's rows again.
        Step("regrow", (Act("resize", "132x60"),), (Cond("prompt", client_only=True),)),
        Step("regrown-type", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
    ), sessions={"a": (132, 60)}),
    Scenario("pane-small", "an ssh terminal SMALLER than the console (80x24): held", (
        Step("ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        Step("type-long", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
        Step("edit-open", (cmd("edit edit.txt"),), (Cond("has", "edit line 01"),)),
        Step("edit-quit", (keys("F3"),), (PROMPT,)),
    ), sessions={"a": (80, 24)}),
    Scenario("help", "help: the whole list (it scrolls), and one long topic", (
        Step("help", (cmd("help"),), (Cond("has", "ver"), PROMPT), timeout=30),
        Step("help-help", (cmd("help help"),), (Cond("has", "EXAMPLES"), PROMPT), timeout=30),
    )),
    Scenario("info", "short informational commands", (
        Step("mode", (cmd("mode"),), (Cond("has", "Available modes"), PROMPT)),
        Step("ver", (cmd("ver"),), (Cond("has", "UEFI Interactive Shell"), PROMPT)),
        Step("date", (cmd("date"),), (PROMPT,)),
        Step("time", (cmd("time"),), (PROMPT,)),
        Step("set", (cmd("set"),), (Cond("has", "path"), PROMPT)),
        Step("alias", (cmd("alias"),), (Cond("has", "dir"), PROMPT)),
        Step("echo-long", (cmd(LONG_ECHO),), (Cond("joined", LONG_ECHO[5:]), PROMPT)),
        Step("error", (cmd("nosuchcmd"),), (Cond("has", "nosuchcmd"), PROMPT)),
    )),
    Scenario("tables", "long and wide tables: memmap, drivers, devices, pci, -b paging", (
        Step("memmap", (cmd("memmap"),), (Cond("has", "Total Memory"), PROMPT), timeout=30),
        Step("drivers", (cmd("drivers"),), (PROMPT,), timeout=30),
        Step("devices", (cmd("devices"),), (PROMPT,), timeout=30),
        Step("pci", (cmd("pci"),), (PROMPT,), timeout=30),
        Step("drivers-b", (cmd("drivers -b"),), (Cond("has", "Press ENTER to continue"),),
             timeout=30),
        Step("drivers-b-next", (keys("Enter"),), (Cond("has", "Press ENTER to continue"),)),
        Step("drivers-b-quit", (typed("q"),), (PROMPT,)),
    )),
    Scenario("files", "a file wider than the console, and ls -r of a deep tree", (
        Step("type-wide", (cmd("type wide.txt"),), (Cond("has", "wide 12"), PROMPT)),
        Step("rm-tree", (cmd("rm -q cvtree"),), (PROMPT,)),
        Step("mkdir", (cmd("mkdir cvtree cvtree\\d1 cvtree\\d1\\d2 cvtree\\d1\\d2\\d3"),),
             (PROMPT,)),
        Step("cp", (cmd("cp -q long.txt cvtree\\d1\\d2\\d3"),), (PROMPT,)),
        Step("ls-r", (cmd("ls -r cvtree"),), (Cond("has", "long.txt"), PROMPT), timeout=30),
    )),
    Scenario("lineedit", "command-line editing at the prompt, history, tab completion", (
        Step("type", (typed("echo abcdef"),), (Cond("has", "echo abcdef"),)),
        Step("left3-X", (keys("Left", "Left", "Left"), typed("X")),
             (Cond("has", "echo abcXdef"),)),
        Step("home-Y", (keys("Home"), typed("Y")), (Cond("has", "Yecho abcXdef"),)),
        Step("delete", (keys("DC"),), (Cond("has", "Ycho abcXdef"),)),
        Step("end-bs", (keys("End", "BSpace", "BSpace"),), (Cond("has", "cho abcXd"),
                                                           Cond("gone", "abcXde"))),
        # Insert toggles the line editor to overwrite: ZZ replaces "Yc".
        Step("insert-toggle", (keys("Home", "IC"), typed("ZZ")),
             (Cond("has", "ZZho abcXd"),)),
        Step("escape", (keys("Escape"),), (PROMPT, Cond("gone", "ZZcho"))),
        Step("run", (cmd("echo history-one"),), (Cond("line", "history-one"), PROMPT)),
        Step("run2", (cmd("echo history-two"),), (Cond("line", "history-two"), PROMPT)),
        Step("up", (keys("Up"),), (Cond("cursorline", "> echo history-two"),)),
        Step("up-up", (keys("Up"),), (Cond("cursorline", "> echo history-one"),)),
        Step("down", (keys("Down"),), (Cond("cursorline", "> echo history-two"),)),
        Step("down-clear", (keys("Escape"),), (PROMPT,)),
        Step("tab", (typed("type lon"), keys("Tab")), (Cond("has", "type long.txt"),)),
        Step("tab-clear", (keys("Escape"),), (PROMPT,)),
    )),
    Scenario("pane-resize", "an ssh terminal that starts SMALLER (80x24) and is enlarged", (
        Step("small-ls", (cmd("ls"),), (Cond("has", "Dir(s)"),)),
        # The console does not change: only the client is repainted.
        Step("enlarge", (Act("resize", "132x60"),), (Cond("prompt", client_only=True),)),
        Step("ver", (cmd("ver"),), (Cond("has", "UEFI Interactive Shell"), PROMPT)),
    ), sessions={"a": (80, 24)}),
    # Output that SCROLLS on a coloured background: each line scrolled in is
    # painted in the background (the GOP is blue throughout).
    Scenario("colour-bg", "scrolling tables and paging on a blue background (cls 1)", (
        Step("cls-1", (cmd("cls 1"),), (Cond("gone", "cv-colour-bg-"), PROMPT)),
        Step("drivers", (cmd("drivers"),), (PROMPT,), timeout=30),
        Step("help-b-page1", (cmd("help -b"),), (Cond("has", "Press ENTER to continue"),)),
        Step("help-b-page2", (keys("Enter"),), (Cond("has", "Press ENTER to continue"),)),
        Step("help-b-quit", (typed("q"),), (PROMPT,)),
        Step("type-long", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
        Step("cls-0", (cmd("echo pre-cls0"), cmd("cls 0")), (Cond("gone", "pre-cls0"), PROMPT)),
    )),
    # The console's geometry changes under the session: `mode` reshapes it,
    # sshd confines the (now larger) terminal to it, and the oracle follows
    # the guest's size. {boot} is the size the guest booted at.
    Scenario("mode", "mode 80 25 at runtime, long output in it, then back", (
        Step("mode-80x25", (cmd("mode 80 25"),), (Cond("gone", "cv-mode-"), PROMPT)),
        Step("type-long", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
        Step("ls", (cmd("ls"),), (Cond("has", "Dir(s)"), PROMPT)),
        Step("mode-back", (cmd("mode {boot}"),), (Cond("gone", "Dir(s)"), PROMPT)),
        Step("type-long-back", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
    )),
    # A codepoint a terminal draws in two cells is one cell on the console:
    # sshd sends '?' for it, so a Shell redraw (scrollback) lands on the
    # console's grid. The serial oracle on a VT-UTF8 terminal widens it (Q3).
    # On a VT100 terminal (aa64) TerminalDxe sends the codepoint's LOW BYTE
    # when that is printable ASCII and '?' otherwise -- U+4E2D goes out as
    # '-' -- so the characters here are ones whose low byte is not printable
    # (U+65E5 0xE5, U+6587 0x87): there the oracle shows '?' too, as sshd.
    # The EDK2 defect: TerminalConOut.c casts to CHAR8 BEFORE testing
    # TerminalIsValidAscii (edk2-stable202505 lines 233-235); docs/ROADMAP.md
    # "Upstream EDK2 defect" has the detail.
    Scenario("wide", "CJK text, then the Shell's scrollback redraws it", (
        Step("type-long", (cmd("type long.txt"),),
             (Cond("has", "long line 120"), PROMPT), timeout=30),
        Step("echo-cjk", (cmd("echo \u65e5\u6587x"),),
             (Cond("line", "??x", client_only=True), PROMPT)),
        Step("echo-after", (cmd("echo after-cjk"),), (Cond("line", "after-cjk"), PROMPT)),
        Step("pgup", (keys("PPage"),), (Cond("gone", "after-cjk"),)),
        Step("pgdn", (keys("NPage"),), (Cond("line", "after-cjk"),)),
        Step("echo-end", (cmd("echo end-cjk"),), (Cond("line", "end-cjk"), PROMPT)),
    )),
    # Last of the ordinary cases: a cleared screen leaves little to tell a
    # step's output from the one before, so each cls is preceded by a marker
    # the step then waits to see gone.
    Scenario("cls", "cls, cls with a colour, then typing on the cleared screen", (
        Step("cls", (cmd("echo pre-cls"), cmd("cls")), (Cond("gone", "pre-cls"), PROMPT)),
        Step("type-after", (cmd("echo after-cls"),), (Cond("line", "after-cls"), PROMPT)),
        Step("cls-1", (cmd("cls 1"),), (Cond("gone", "after-cls"), PROMPT)),
        Step("ver-on-blue", (cmd("ver"),), (Cond("has", "UEFI Interactive Shell"), PROMPT)),
        Step("cls-4", (cmd("cls 4"),), (Cond("gone", "UEFI Interactive Shell"), PROMPT)),
        Step("cls-0", (cmd("echo pre-cls0"), cmd("cls 0")), (Cond("gone", "pre-cls0"), PROMPT)),
        Step("typed-unsent", (typed("half typed"),), (Cond("has", "half typed"),)),
        Step("esc-clears", (keys("Escape"),), (PROMPT, Cond("gone", "half typed"))),
    )),
    Scenario("exit", "exit ends the Shell for every session", (
        # The goodbye is the last thing a session sees: nothing the firmware
        # prints after the Shell has gone (the boot manager) reaches it.
        Step("exit", (cmd("exit"),), (Cond("dead", "a", client_only=True),
                                      Cond("has", "[sshd] the UEFI Shell exited",
                                           client_only=True),
                                      Cond("gone", "BdsDxe", client_only=True))),
    ), destructive=True),
)



def selftest() -> list[str]:
    """The known-differences table must classify only what it names. Q1 is
    TerminalDxe's stale last-column cell; a margin regression of our own (the
    encoder dropping or misplacing the cell it wraps at) must stay
    unclassified. Returns the failures (no QEMU)."""
    import pyte  # pyright: ignore[reportMissingTypeStubs]

    from cvlib.diff import compare
    from cvlib.grid import BceScreen, Grid, grid_from_screen

    def grid(rows: list[str]) -> Grid:
        screen = BceScreen(10, 3)
        stream = pyte.Stream(screen)
        for y, text in enumerate(rows):
            stream.feed(f"\x1b[{y + 1};1H{text}")
        stream.feed("\x1b[3;1H")
        return grid_from_screen(screen)

    q1_ids = {"Q1-terminaldxe-lastcol"}
    cases: list[tuple[str, list[str], list[str], bool]] = [
        # The genuine quirk: a wrapped line, the oracle's last cell repeats
        # the one before it, the client's is the real character.
        ("quirk", ["abcdefgh11", "345"], ["abcdefgh12", "345"], True),
        # The encoder dropping the cell it wraps at: the client's last column
        # is blank -- against a correct oracle, or against the quirk.
        ("dropped", ["abcdefgh12", "345"], ["abcdefgh1", "345"], False),
        ("dropped vs quirk", ["abcdefgh11", "345"], ["abcdefgh1", "345"], False),
        # A wrong last cell that is not the quirk's repeat.
        ("wrong char", ["abcdefgh12", "345"], ["abcdefgh1X", "345"], False),
        # No continuation: a line that does not wrap is not the quirk.
        ("no wrap", ["abcdefgh11"], ["abcdefgh12"], False),
        # The continuation differs: a misplaced wrap, not the quirk.
        ("moved wrap", ["abcdefgh11", "345"], ["abcdefgh12", "45"], False),
        # The rest of the row differs too.
        ("row differs", ["abcdefgX11", "345"], ["abcdefgh12", "345"], False),
    ]
    fails: list[str] = []
    for name, oracle_rows, client_rows, want in cases:
        res = compare(grid(oracle_rows), grid(client_rows), KNOWN)
        last = [d for d in res.diffs if d.x == 9 and d.y == 0]
        got = bool(last) and last[0].known in q1_ids
        if got != want:
            fails.append(f"Q1 {name}: want classified={want}, got {got}")
    fails += _geometry_selftest()
    q3 = {"Q3-terminaldxe-wide"}
    for name, o_row, c_row, want in (
            # A widened glyph with its continuation cell, and one whose
            # continuation the next character overwrote (the line editor).
            ("output row", "\u4e2d\u6587x", "??x", True),
            ("echoed row", "> \u4e2d\x1b[1;4H\u6587\x1b[1;5Hx", "> ??x", True),
            # Near misses: a cell lost, a wrong character, no wide glyph.
            ("dropped", "\u4e2d\u6587x", "?x", False),
            ("wrong", "\u4e2d\u6587x", "??y", False),
            ("no wide", "abc", "a?c", False)):
        res = compare(grid([o_row]), grid([c_row]), KNOWN)
        got = bool(res.diffs) and all(d.known in q3 for d in res.diffs)
        if got != want:
            fails.append(f"Q3 {name}: want classified={want}, got {got}")
    return fails


def _geometry_selftest() -> list[str]:
    """The serial oracle follows sshd's geometry report: resized at the
    report, before what follows it is drawn, and the report itself drawn
    nowhere -- even when a read splits it at any byte; the last of several
    reshapes wins; and a reshape written to the console fails the run."""
    from cvlib.oracles import GeometryFeeder

    fails: list[str] = []
    rec = b"\x1b_axl-console-size=80x25\x1b\\"
    clear = b"\x1b[2J\x1b[01;01H"
    head = b"FS0:\\> mode 80 25\r\n"

    def size(f: GeometryFeeder) -> tuple[int, int]:
        return f.screen.columns, f.screen.lines

    plain = GeometryFeeder(100, 31, None)
    plain.feed(rec + clear)
    if size(plain) != (100, 31):
        fails.append(f"geometry: with no pattern the screen moved to {size(plain)}")
    for cut in range(len(rec) + 1):
        f = GeometryFeeder(100, 31, GEOMETRY_RE)
        # No clear after the report: what the report would draw stays seen.
        f.feed(head + rec[:cut])
        f.feed(rec[cut:] + b"\x1b[25;80Hz")
        text = "".join(f.screen.display)
        if size(f) != (80, 25):
            fails.append(f"geometry: split at {cut}: screen {size(f)}, want (80, 25)")
        elif f.screen.display[24][79] != "z":
            fails.append(f"geometry: split at {cut}: the last cell is not drawn")
        elif "axl" in text or "80x25" in text:
            fails.append(f"geometry: split at {cut}: the report was drawn: {text.strip()!r}")
        elif f.screen.display[0].rstrip() != "FS0:\\> mode 80 25":
            fails.append(f"geometry: split at {cut}: row 0 is {f.screen.display[0].rstrip()!r}")
    # Two reshapes in one read, and one more in the next: the LAST one wins.
    f = GeometryFeeder(100, 31, GEOMETRY_RE)
    f.feed(rec + clear + rec.replace(b"80x25", b"100x31"))
    if size(f) != (100, 31):
        fails.append(f"geometry: two reports in one read ended at {size(f)}")
    f.feed(b"more output" + rec.replace(b"80x25", b"80x50"))
    if size(f) != (80, 50):
        fails.append(f"geometry: a third report in a later read ended at {size(f)}")
    # An escape that is NOT a report is held only until it is complete.
    f = GeometryFeeder(100, 31, GEOMETRY_RE)
    f.feed(b"ab\x1b")
    f.feed(b"[1;5Hc")
    if f.screen.display[0].rstrip() != "ab  c":
        fails.append(f"geometry: a split CSI drew {f.screen.display[0].rstrip()!r}")
    # The old console line: the feeder flags it (control: a report does not).
    for data, want in ((b"x 12:00:00 [INFO]  sshd: the console is ", True), (rec, False)):
        f = GeometryFeeder(100, 31, GEOMETRY_RE, CONSOLE_GEOMETRY_RE)
        f.feed(data)
        f.feed(b"now 80x25\r\n" if want else b"")
        if (f.forbidden is not None) != want:
            fails.append(f"geometry: forbidden={f.forbidden!r} for {data!r}, want {want}")
    return fails


SUITE = Suite(
    name="sshd",
    what="the live UEFI Shell over SSH (tools/sshd.c): ssh -tt in tmux vs TerminalDxe vs GOP",
    build=build, boot=boot, console_size=console_size, client=client,
    grid_oracle=lambda guest, cols, rows: SerialPyteOracle(
        guest, cols, rows, GEOMETRY_RE, CONSOLE_GEOMETRY_RE), picture_oracle=FirmwareShot, prompt=PROMPT_RE,
    start=(cmd("cls"),), mark=mark, cases=CASES, known=KNOWN, held=HELD_RE,
    selftest=selftest)
