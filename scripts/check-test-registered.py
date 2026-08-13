#!/usr/bin/env python3
"""check-test-registered.py — flag a unit test that is defined but never run.

A `static void test_foo(void)` in `test/unit/*.c` that no call site invokes is
invisible to everything: it compiles (gcc's -Wunused-function does not reliably
fire on it, and the tree's gcc build is warning-clean today), it prints nothing,
and the pass-count ratchet cannot see a test that never ran -- the count simply
never goes up, which looks identical to "no test was added". The test reads as
coverage in review and provides none.

This gate cross-checks every `test_*` DEFINITION in a unit-test translation unit
against the references to that name within the same TU (the .c plus the shared
test/unit headers it can see). A definition with zero surviving references is an
orphan and fails the gate.

What counts as "registered" is deliberately broader than a direct call, because
tests reach their entry points in several legitimate ways:
  - a plain call from the suite's main:     test_foo();
  - a function pointer in a vtable/table:   .write = test_sink_write,
  - the app entry macro:                    AXL_APP(test_io_main)
So the rule is "the identifier is mentioned somewhere other than its own
definition". Comments and string literals are stripped first -- a name that only
appears inside a `test_check(..., "test_foo")` label or a comment is NOT a
registration, and that is exactly the shape a stale orphan leaves behind.

A forward declaration does not count either. It is a promise, not a use; letting
it register the test would make `static void test_foo(void);` a way to silence
this gate without ever running anything.

Usage: scripts/check-test-registered.py   (run from anywhere)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UNIT_DIR = ROOT / "test" / "unit"

# Function definitions in this tree put the name at column 0 on its own line,
# under the return type (the multi-line-signature house style):
#     static void
#     test_foo(void)
#     {
DEF_RE = re.compile(r"^(test_[A-Za-z0-9_]+)\s*\(", re.MULTILINE)

# A declarator's prefix is only type tokens: identifiers, whitespace, stars.
# This is what separates `static void test_foo(void);` (a declaration) from
# `test_foo();` (a call, whose prefix is blank) and from `x = test_foo();`
# (an expression, whose prefix contains '='). Both statements end in ';', so
# the prefix is the ONLY discriminator -- an earlier version of this script
# matched calls as declarations and reported every test in the tree as an
# orphan.
TYPE_PREFIX = re.compile(r"^[ \t]*[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]$")
TYPE_LINE = re.compile(r"^[ \t]*[A-Za-z_][A-Za-z0-9_ \t*]*$")


def strip_noise(text: str) -> str:
    """Blank comments and literals, preserving newlines so line math survives.

    A test name mentioned only in a comment or in a `test_check` label string is
    not a call site -- counting it would let a deleted call site hide behind the
    label it left behind.

    This is a single left-to-right scan rather than four independent regex
    passes, because the four constructs are mutually exclusive and a regex pass
    cannot know that. Stripping block comments first made a `"/*"` inside a
    STRING (the JSON5 comment tests are full of them) open a comment that ran to
    the next `*/` and blanked hundreds of lines of real code -- silently
    disabling the gate over exactly those files.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif ch == "/" and nxt == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif ch in ('"', "'"):
            j = i + 1
            while j < n and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def close_paren(text: str, open_idx: int) -> int:
    """Index of the ')' matching the '(' at @a open_idx, or -1."""
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def is_declarator(text: str, start: int) -> bool:
    """True when the occurrence at @a start is preceded only by a type.

    Covers both `static void test_foo(void);` (type on the same line) and the
    house multi-line style, where the name sits at column 0 with the return
    type on the line above.
    """
    line_start = text.rfind("\n", 0, start) + 1
    prefix = text[line_start:start]
    if TYPE_PREFIX.match(prefix):
        return True
    if prefix.strip():
        return False                      # an expression, e.g. `x = test_foo()`
    prev_end = line_start - 1             # the '\n' before this line
    if prev_end <= 0:
        return False
    prev_start = text.rfind("\n", 0, prev_end) + 1
    return bool(TYPE_LINE.match(text[prev_start:prev_end]))


IDENT_RE = re.compile(r"(?<![A-Za-z0-9_])(test_[A-Za-z0-9_]+)(?![A-Za-z0-9_])")


def classify_all(text: str) -> dict[str, tuple[int, int, int]]:
    """Map every `test_*` name in @a text to (definitions, declarations, refs).

    One scan for all names rather than one scan per name -- with ~1700 tests the
    per-name form re-walked the whole corpus 1700 times.
    """
    counts: dict[str, list[int]] = {}
    n = len(text)

    def skip_space(idx: int) -> int:
        while idx < n and text[idx].isspace():
            idx += 1
        return idx

    for match in IDENT_RE.finditer(text):
        slot = counts.setdefault(match.group(1), [0, 0, 0])
        open_idx = skip_space(match.end())
        if open_idx >= n or text[open_idx] != "(":
            slot[2] += 1                  # bare mention: a function pointer
            continue
        close = close_paren(text, open_idx)
        if close < 0:
            slot[2] += 1
            continue
        after = skip_space(close + 1)
        tail = text[after] if after < n else ""
        declarator = is_declarator(text, match.start())
        if declarator and tail == "{":
            slot[0] += 1
        elif declarator and tail == ";":
            slot[1] += 1
        else:
            slot[2] += 1                  # a call
    return {k: (v[0], v[1], v[2]) for k, v in counts.items()}


def main() -> int:
    headers = "\n".join(
        strip_noise(p.read_text(encoding="utf-8", errors="replace"))
        for p in sorted(UNIT_DIR.glob("*.h"))
    )

    orphans: list[str] = []
    total = 0

    for src in sorted(UNIT_DIR.glob("*.c")):
        raw = src.read_text(encoding="utf-8", errors="replace")
        body = strip_noise(raw)
        # The TU as the compiler sees it: this .c plus the shared test headers.
        scope = body + "\n" + headers

        counts = classify_all(scope)
        for match in DEF_RE.finditer(body):
            name = match.group(1)
            ndef, _ndecl, nref = counts.get(name, (0, 0, 0))
            if ndef == 0:
                continue                  # a prototype only; no body to orphan
            total += 1
            if nref == 0:
                line = body.count("\n", 0, match.start()) + 1
                orphans.append(
                    f"  {src.relative_to(ROOT)}:{line}: {name} is defined but "
                    f"never called")

    if orphans:
        print("check-test-registered: FAIL — unit test(s) defined but never "
              "run:")
        print("\n".join(orphans))
        print("\n  An unregistered test reports nothing and cannot move the "
              "pass-count\n  ratchet, so it reads as coverage while providing "
              "none. Either add the\n  call to the suite's main(), or delete "
              "the function.")
        return 1

    print(f"check-test-registered: clean ({total} unit tests, all reachable).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
