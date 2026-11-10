#!/usr/bin/env python3
"""subreaper.py -- run a command as a Linux child subreaper.

Usage: subreaper.py PROGRAM [ARG...]

Marks this process PR_SET_CHILD_SUBREAPER, then execs PROGRAM in its place
(the mark survives exec). A descendant whose parent exits is then reparented
to PROGRAM instead of to init, so when it ends PROGRAM reaps it and its CPU
lands in PROGRAM's cutime/cstime. lib/run-one.sh runs itself this way: a
`run-qemu.sh --background` guest's watchdog outlives the run-qemu.sh that
started it, and without this its QEMU's CPU went to init, never to the test
that paid for it (`TIMING: runner ... cpu=`).

Where prctl is unavailable (not Linux, no libc), PROGRAM runs anyway and the
reason goes to stderr: the CPU reading is then a floor, said, not a refusal
to run the test.
"""
from __future__ import annotations

import ctypes
import os
import sys

PR_SET_CHILD_SUBREAPER = 36


def mark_subreaper() -> str | None:
    """Mark this process a child subreaper; None, or why it could not be."""
    if not sys.platform.startswith("linux"):
        return f"not Linux ({sys.platform})"
    try:
        libc = ctypes.CDLL(None, use_errno=True)
    except OSError as e:
        return f"no libc: {e}"
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        return f"prctl(PR_SET_CHILD_SUBREAPER): {os.strerror(ctypes.get_errno())}"
    return None


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: subreaper.py PROGRAM [ARG...]", file=sys.stderr)
        return 2
    why = mark_subreaper()
    if why is not None:
        print(f"subreaper: {why}; orphaned descendants' CPU will not be counted",
              file=sys.stderr)
    # run-one.sh's opt-out of PEP 538 locale coercion is ours, not PROGRAM's.
    os.environ.pop("PYTHONCOERCECLOCALE", None)
    os.execvp(argv[0], argv)
    return 127   # not reached: execvp raises on failure


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
