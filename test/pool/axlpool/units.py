"""units.py -- the systemd user units that run the pool as a socket-activated
service (service.py's PROCESS): `python3 -m axlpool units`.

The units are generated, not shipped, because ExecStart names absolute
paths -- the interpreter and the service's tree (whose agent builds it
pins) -- that
only the machine installing them knows. They are written to a directory;
installing them into ~/.config/systemd/user is a separate, printed step.
"""
from __future__ import annotations

import re
from pathlib import Path

from axlpool.service import EXIT_CANNOT_START, STOP_BOUND_S

EXIT_IDLE_S = 900                       # the service's own default
STOP_SLACK_S = 30                       # TimeoutStopSec beyond the service's own bound
EXIT_USAGE = 2                          # argparse: a bad command line never fixes itself
_UNSAFE = re.compile(r"""[\s"'\\%$]""")  # what a unit line would split, quote or expand


class UnitError(ValueError):
    """The units cannot be written as asked."""


def _safe(p: Path) -> str:
    s = str(p)
    if _UNSAFE.search(s):
        raise UnitError(f"{s}: a path with whitespace, quotes, backslashes, % or $ cannot go in "
                        f"a unit unescaped")
    return s


def render(tree: Path, python: Path) -> dict[str, str]:
    """{file name: text} of axlpool.socket and axlpool.service for a service
    of @tree -- pinning, per arch, the agent @tree builds -- run by @python.
    Raises UnitError."""
    if not (tree / "test" / "pool" / "classes.toml").is_file():
        raise UnitError(f"{tree} has no test/pool/classes.toml: not an axl-sdk tree")
    tree_s = _safe(tree)                     # named first: it is what the user passed
    pool_dir = _safe(tree / "test" / "pool")
    # --state and --socket spelled out as the manager's own directories (%S,
    # %t), so the unit never depends on what XDG_* the user manager has.
    exec_start = (f"{_safe(python)} -m axlpool serve --tree {tree_s} "
                  f"--state %S/axlpool --socket %t/axlpool/pool.sock --exit-idle {EXIT_IDLE_S}")
    socket = "\n".join([
        "[Unit]",
        "Description=AXL test guest pool (socket)",
        "",
        "[Socket]",
        "ListenStream=%t/axlpool/pool.sock",
        "SocketMode=0600",
        "DirectoryMode=0700",
        "Accept=no",
        "",
        "[Install]",
        "WantedBy=sockets.target",
    ]) + "\n"
    service = "\n".join([
        "[Unit]",
        "Description=AXL test guest pool",
        "Requires=axlpool.socket",
        "After=axlpool.socket",
        "StartLimitIntervalSec=300",
        "StartLimitBurst=5",
        "",
        "[Service]",
        "Type=simple",
        f"WorkingDirectory={pool_dir}",
        f"ExecStart={exec_start}",
        "KillMode=mixed",                      # SIGTERM to the service only: its ordered stop runs
        f"TimeoutStopSec={int(STOP_BOUND_S) + STOP_SLACK_S}",
        "Restart=on-failure",
        f"RestartPreventExitStatus={EXIT_USAGE} {EXIT_CANNOT_START}",
        "RestartSec=2",
    ]) + "\n"
    return {"axlpool.socket": socket, "axlpool.service": service}


def write(out: Path, units: dict[str, str]) -> list[Path]:
    """Write @units into @out (created); the paths written, in order."""
    out.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for name, text in units.items():
        (out / name).write_text(text)
        written.append(out / name)
    return written
