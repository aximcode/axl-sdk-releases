"""The shape of a case table, and of the suite that owns it.

A SUITE is one component under test: how to build and boot it, which client
shows what its user sees, which oracles to hold that against, its scenarios
and its known-differences list. A suite is one module in ``cases/`` that
defines ``SUITE``; ``console-visual.py --suite NAME`` loads it.

A SCENARIO opens its own client session(s) and starts from a known screen
(the suite's ``start`` actions plus a per-scenario marker), so
``--only=NAME`` re-runs it alone. A STEP is: actions on one session -> wait
until its conditions hold on the client AND on the grid oracle (polled every
50 ms, bounded; never a sleep) -> wait until both stop changing -> capture
-> diff.
"""
from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from cvlib.clients import Client, Tmux
from cvlib.diff import Known
from cvlib.guest import BootSpec, Guest
from cvlib.oracles import GridOracle, PictureOracle

# A session size: None = the console's own size; else (cols, rows).
Size = tuple[int, int] | None


@dataclass(frozen=True)
class Act:
    """One action on a session.

    Runner-level kinds: ``open`` (start session ``arg``), ``cmd`` (type
    ``arg`` then Enter; ``{boot}`` in it is the console size the guest booted
    at, as ``COLS ROWS``), ``keylat`` (type ``arg`` one key at a time, timing
    each echo on the client and the oracle). Anything else is handed to the
    client's ``act`` (TmuxClient: ``type``, ``keys``, ``paste``).
    """

    kind: str
    arg: str | tuple[str, ...] = ""
    on: str = "a"


@dataclass(frozen=True)
class Cond:
    """A content predicate, checked on the client AND the grid oracle
    (``client_only`` for what the oracle cannot show, e.g. a closed session).

    Kinds: ``line`` (a row is exactly arg), ``has`` (a row contains arg),
    ``gone`` (no row contains arg), ``joined`` (the rows run together contain
    arg -- for wrapped lines), ``prompt`` (the text left of the cursor matches
    the suite's prompt), ``cursorline`` (the text left of the cursor contains
    arg -- the line being edited), ``dead`` (session arg's client has exited).
    """

    kind: str
    arg: str = ""
    client_only: bool = False


@dataclass(frozen=True)
class Step:
    name: str
    acts: tuple[Act, ...]
    until: tuple[Cond, ...]
    timeout: float = 10.0
    # Wait for both grids to stop changing before capturing (off for a step
    # whose screen keeps moving, e.g. a running loop).
    settle: bool = True
    # > 0: a step that shows output STOPPING. It completes once its
    # conditions hold and the client and the oracle have both been unchanged
    # this many seconds -- no change since the action is required.
    still: float = 0.0


@dataclass(frozen=True)
class Scenario:
    name: str
    what: str
    steps: tuple[Step, ...]
    # Sessions this scenario may open; "a" is opened at its start, others
    # by an ``open`` act.
    sessions: dict[str, Size] = field(default_factory=lambda: {"a": None})
    # Leaves the guest unusable (e.g. exits the Shell): run last, and never
    # against a kept guest unless named in --only.
    destructive: bool = False


@dataclass
class ClientContext:
    """What a suite's client factory gets from the runner."""

    tmux: Tmux
    guest: Guest
    name: str
    size: tuple[int, int]
    raw_log: Path


@dataclass
class Suite:
    name: str
    what: str
    build: Callable[[str], None]  # arch
    boot: Callable[[str, Path], BootSpec]  # arch, workdir (stage fixtures there)
    console_size: Callable[[Guest], tuple[int, int]]
    client: Callable[[ClientContext], Client]
    grid_oracle: Callable[[Guest, int, int], GridOracle]
    picture_oracle: Callable[[Guest, int, int], PictureOracle] | None
    prompt: re.Pattern[str]
    # The actions that bring a session to a known screen; the runner appends
    # ``cmd <marker>``-style checks via ``mark``.
    start: tuple[Act, ...]
    mark: Callable[[str], tuple[tuple[Act, ...], tuple[Cond, ...]]]
    cases: tuple[Scenario, ...]
    known: list[Known]
    # What a client shows when the component deliberately holds it (e.g. a
    # terminal too small for the console): a held client counts as opened,
    # and meets every condition that is not client_only.
    held: re.Pattern[str] | None = None
    # Checks of the suite's own tables (its known-differences matchers) that
    # need no guest; run by --selftest. Returns the failures.
    selftest: Callable[[], list[str]] | None = None


def cmd(text: str, on: str = "a") -> Act:
    return Act("cmd", text, on)


def keys(*names: str, on: str = "a") -> Act:
    return Act("keys", tuple(names), on)


def typed(text: str, on: str = "a") -> Act:
    return Act("type", text, on)


PROMPT = Cond("prompt")
