"""checks.py -- the PASS/FAIL reporting the Python case scripts share.

Every check compares WHOLE values (the repo's exact-assertion rule): a
substring or a membership test would let the regressions they guard pass.
"""
from __future__ import annotations

from pathlib import Path


def ppid(pid: int) -> int:
    """@pid's parent, from /proc (field 4 of stat, after the parenthesised
    comm); -1 when it cannot be read."""
    try:
        return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[1])
    except (OSError, ValueError, IndexError):
        return -1


class Cases:
    """Counts and prints `  PASS: <name>` / `  FAIL: <name>: got .., want ..`."""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, name: str, got: object, want: object) -> None:
        if got == want:
            print(f"  PASS: {name}")
            self.passed += 1
        else:
            print(f"  FAIL: {name}: got {got!r}, want {want!r}")
            self.failed += 1
