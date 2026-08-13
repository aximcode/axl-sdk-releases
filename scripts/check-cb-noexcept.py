#!/usr/bin/env python3
"""Every public callback declaration must carry AXL_CB_NOEXCEPT.

AXL invokes consumer callbacks from its own C frames, which are compiled
-fno-exceptions and carry no landing pads: an exception unwinding through
them runs no cleanup at all, leaking every AXL_AUTO_FREE in the path and
— worse — any RaiseTPL, which wedges the machine on return to the shell.
AXL_CB_NOEXCEPT makes that a compile error for C++ consumers.

The compile fixture in `make check-cb-noexcept` proves the macro REJECTS a
throwing callback, but it can only speak for the two declarations it
names. This is the structural half: it fails when a NEW callback is added
without the macro, which the fixture would happily pass.

Two shapes are checked:

    typedef int (*AxlFooFn)(void *ctx);      a callback TYPE
        int    (*handler)(void *ctx);        a callback SLOT in a vtable

and one is deliberately NOT: an AXL function that merely takes a callback
parameter. There the ';' belongs to the enclosing prototype, so marking it
would declare AXL's own function noexcept and enforce nothing on the
callback.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

TYPEDEF = re.compile(
    r"typedef\s[^;]*?\(\s*\*\s*(?P<name>Axl[A-Za-z0-9_]+)\s*\)\s*\(")
MEMBER = re.compile(
    r"^[ \t]+[A-Za-z_][A-Za-z_0-9 ]*\*?\s*\(\s*\*\s*(?P<name>[a-z_][a-z_0-9]*)\s*\)\s*\(",
    re.M)
COMMENT = re.compile(r"///<[^\n]*|/\*.*?\*/|//[^\n]*", re.S)


def only_trivia(s: str) -> bool:
    """Whitespace, comments, and the macro itself.

    The macro MUST be stripped here. Without it every already-marked
    declaration looks like a callback parameter (its ';' no longer
    'immediately follows' the paren), so the scan skips it — the gate
    then reports zero marked declarations while still passing, which
    reads as coverage and is the opposite.
    """
    return COMMENT.sub("", s).replace("AXL_CB_NOEXCEPT", "").strip() == ""


def match_paren(text: str, start: int) -> int:
    depth = 0
    for j in range(start, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def scan(text: str, pattern: re.Pattern[str]) -> tuple[list[str], int]:
    """Return (names missing the macro, number of marked declarations)."""
    missing: list[str] = []
    marked = 0
    i = 0
    while True:
        m = pattern.search(text, i)
        if m is None:
            return missing, marked
        close = match_paren(text, m.end() - 1)
        if close < 0:
            return missing, marked
        semi = text.find(";", close)
        if semi < 0:
            return missing, marked
        if only_trivia(text[close + 1:semi]):
            if "AXL_CB_NOEXCEPT" in text[close:semi]:
                marked += 1
            else:
                line = text.count("\n", 0, m.start()) + 1
                missing.append(f"{m.group('name')} (line {line})")
        i = close + 1


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "include/axl")
    total = 0
    bad: list[str] = []
    for path in sorted(root.glob("*.h")):
        text = path.read_text(errors="replace")
        for pat in (TYPEDEF, MEMBER):
            missing, marked = scan(text, pat)
            total += marked
            bad += [f"{path}: {n}" for n in missing]

    if bad:
        print("check-cb-noexcept: FAIL — callback declarations without "
              "AXL_CB_NOEXCEPT:")
        for b in bad:
            print(f"    {b}")
        print("\n  A callback AXL invokes must promise not to throw: the C")
        print("  frames it would unwind through run no cleanup. Append")
        print("  AXL_CB_NOEXCEPT after the parameter list, and #include")
        print("  <axl/axl-macros.h> if the header does not already.")
        return 1

    print(f"check-cb-noexcept: clean — {total} callback declarations carry "
          f"AXL_CB_NOEXCEPT")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
