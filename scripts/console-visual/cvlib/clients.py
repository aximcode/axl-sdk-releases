"""Clients under test: what a USER of the component sees.

A client is anything that can be opened at a size, driven with input, and
read back as a Grid. ``TmuxClient`` runs any command (``ssh -tt ...`` for
sshd; a serial-socket reader or a host-side renderer for another component)
in a tmux pane of the chosen size, and reads the pane back with
``capture-pane``: tmux is the terminal emulator, so the grid is what a
person at an xterm-compatible terminal would see.

Adding a client: implement the ``Client`` protocol (``open``, ``act``,
``grid``, ``dead``, ``close``, ``save``). The runner never looks inside one.
"""
from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

import pyte  # pyright: ignore[reportMissingTypeStubs]

from cvlib.grid import BceScreen, Grid, grid_from_screen


class Client(Protocol):
    """A pluggable client under test."""

    name: str
    tag: str  # file-name tag of its captures ("ssh" -> NN-step.ssh.txt)
    cols: int
    rows: int

    def open(self) -> None: ...
    def act(self, kind: str, arg: str | tuple[str, ...]) -> None: ...
    def grid(self) -> Grid: ...
    def dead(self) -> bool: ...
    def close(self) -> None: ...
    def save(self, stem: Path) -> list[str]: ...
    def describe(self) -> str: ...


class Tmux:
    """One private tmux server (its own socket) for a run's panes."""

    def __init__(self, name: str, conf: Path) -> None:
        self.name = name
        conf.write_text("\n".join([
            "set -g status off",
            # What a typical user's terminal is; a client such as ssh(1)
            # forwards it to the far side as TERM.
            "set -g default-terminal xterm-256color",
            "set -g remain-on-exit on",
            "set -g history-limit 10000",
            "set -g escape-time 0", ""]))
        self.conf = conf

    def run(self, *args: str, check: bool = True) -> str:
        res = subprocess.run(["tmux", "-L", self.name, "-f", str(self.conf), *args],
                             capture_output=True, text=True)
        if check and res.returncode != 0:
            raise RuntimeError(f"tmux {' '.join(args)}: {res.stderr.strip()}")
        return res.stdout

    def kill_server(self) -> None:
        """Kill the server and remove its socket file (tmux leaves it)."""
        sock = self.run("display", "-p", "#{socket_path}", check=False).strip()
        self.run("kill-server", check=False)
        if not sock:
            tmpdir = os.environ.get("TMUX_TMPDIR", "/tmp")
            sock = f"{tmpdir}/tmux-{os.getuid()}/{self.name}"
        Path(sock).unlink(missing_ok=True)

    def sessions(self) -> list[str]:
        out = self.run("list-sessions", "-F", "#{session_name}", check=False)
        return [s for s in out.splitlines() if s]


@dataclass
class TmuxClient:
    """``command`` in a tmux pane of cols x rows.

    Actions: ``type`` (literal text), ``keys`` (tmux key names: Enter, Up,
    PageDown, F2, C-c, ...), ``paste`` (one paste-buffer write), ``resize``
    (``"COLSxROWS"``: resize the pane, as a user resizing the window).
    """

    tmux: Tmux
    name: str
    command: list[str]
    cols: int
    rows: int
    raw_log: Path
    tag: str = "client"
    sent: list[str] = field(default_factory=lambda: [])

    def open(self) -> None:
        self.raw_log.unlink(missing_ok=True)
        self.tmux.run("new-session", "-d", "-s", self.name, "-x", str(self.cols),
                      "-y", str(self.rows), " ".join(_sh_quote(a) for a in self.command))
        # The raw bytes the command wrote to its terminal: the producer's
        # output, escape for escape, for when a grid alone cannot say why.
        self.tmux.run("pipe-pane", "-o", "-t", self.name,
                      f"cat >> {_sh_quote(str(self.raw_log))}")

    def act(self, kind: str, arg: str | tuple[str, ...]) -> None:
        if kind == "type":
            self.sent.append(f"type {arg!r}")
            self.tmux.run("send-keys", "-t", self.name, "-l", str(arg))
        elif kind == "keys":
            names = arg if isinstance(arg, tuple) else (arg,)
            self.sent.append(f"keys {' '.join(names)}")
            self.tmux.run("send-keys", "-t", self.name, *names)
        elif kind == "paste":
            self.sent.append(f"paste {len(arg)} chars")
            self.tmux.run("set-buffer", "-b", f"p-{self.name}", str(arg))
            self.tmux.run("paste-buffer", "-d", "-b", f"p-{self.name}", "-t", self.name)
        elif kind == "resize":
            # "COLSxROWS": the terminal is resized, as a user drags a window;
            # ssh(1) sends the far side a window-change.
            c, r = (int(v) for v in str(arg).split("x"))
            self.sent.append(f"resize {c}x{r}")
            self.tmux.run("resize-window", "-t", self.name, "-x", str(c), "-y", str(r))
            self.cols, self.rows = c, r
        else:
            raise ValueError(f"TmuxClient: unknown action {kind!r}")

    def dead(self) -> bool:
        out = self.tmux.run("display", "-p", "-t", self.name, "#{pane_dead}", check=False)
        return out.strip() == "1"

    def grid(self) -> Grid:
        sgr = self.capture(sgr=True)
        out = self.tmux.run("display", "-p", "-t", self.name, "#{cursor_x},#{cursor_y}")
        cx, cy = (int(v) for v in out.strip().split(","))
        screen = BceScreen(self.cols, self.rows)
        stream = pyte.Stream(screen)
        # capture-pane -e is STATEFUL across lines: it emits an attribute
        # only when it changes, so the lines must not be reset between.
        for y, line in enumerate(sgr.split("\n")[: self.rows]):
            stream.feed(f"\x1b[{y + 1};1H{line}")
        g = grid_from_screen(screen)
        g.cursor = (cx, cy)
        return g

    def capture(self, sgr: bool) -> str:
        flags = ["-p", "-N"] + (["-e"] if sgr else [])
        return self.tmux.run("capture-pane", *flags, "-t", self.name)

    def close(self) -> None:
        self.tmux.run("kill-session", "-t", self.name, check=False)

    def save(self, stem: Path) -> list[str]:
        Path(f"{stem}.{self.tag}.txt").write_text(self.capture(sgr=False))
        Path(f"{stem}.{self.tag}-sgr.txt").write_text(self.capture(sgr=True))
        return [f"{stem.name}.{self.tag}.txt", f"{stem.name}.{self.tag}-sgr.txt"]

    def describe(self) -> str:
        return f"{self.tag} in tmux {self.cols}x{self.rows}"


def _sh_quote(s: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9_./:=@%+,-]+", s):
        return s
    return "'" + s.replace("'", "'\\''") + "'"
