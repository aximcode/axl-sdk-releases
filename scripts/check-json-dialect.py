#!/usr/bin/env python3
"""check-json-dialect.py -- every JSON parse in library and tool code names
AXL_JSON_STRICT, unless it is explicitly marked as reading a local file.

Design decision 40 (docs/AXL-JSON-Design.md): anything crossing the NETWORK is
strict RFC 8259 in both directions -- an HTTP request or response body, a
WebSocket payload, a Redfish resource -- because a peer that sends JSON5 is
either broken or probing. The liberal dialect is for files AXL reads locally:
the JSON5 sidecars in this tree, config files, and anything a consumer
deliberately opts into.

That rule was written here and enforced only downstream. SoftBMC, which cannot
use axl_http_request_get_json() because it has its own HTTP_REQUEST, built a
source lint for it (tests/json-strict-check.sh) after a mutation build showed
what the liberal dialect cost: a JSON5 `PUT /api/users/admin` carrying
`role: 'readonly'` was accepted and DEMOTED the administrator account. The SDK
that authored the rule had no equivalent check at all -- so the rule held here
by review, which is the arrangement that gets re-broken.

Folding the _flags twins into the base signatures (decision 41) removed the
ACCIDENT -- axl_json_parse() no longer has a liberal default to fall into, and
no call site can get JSON5 without naming it. It did not remove the DECISION.
This gate is about the decision.

THE RULE. Every axl_json_parse / axl_json_load_file / axl_json_parse_source
call under src/ and tools/ must pass AXL_JSON_STRICT, unless the line above the
call carries the marker:

    /* json-dialect: local-file -- <why> */

A marker is a deliberate act that travels with the code and states its reason,
which an allow-list of file:line numbers does neither. There is exactly one in
the tree today (the JSON5 sidecar loader), and that is the shape this expects:
liberal parsing concentrated in one helper the rest of the tree goes through.

Not scanned: test/ (fixtures are neither network nor local config, and the
suite deliberately parses JSON5 to test that it can) and sdk/examples/ (see
check-examples).

Usage: scripts/check-json-dialect.py   (run from anywhere)
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# entry point -> zero-based index of its dialect argument
ENTRIES = {
    "axl_json_parse": 2,          # (json, len, flags, r)
    "axl_json_load_file": 1,      # (path, flags, r, out_buf, out_len)
    "axl_json_parse_source": 1,   # (src, flags, r)
    "axl_json_scanner_init": 2,   # (s, src, flags)
}

REQUIRED = "AXL_JSON_STRICT"
MARKER = "json-dialect: local-file"

SCAN_DIRS = ("src", "tools")

# The JSON module's own implementation is not a policy site: these files
# forward the caller's flags word into the next layer down, so the dialect they
# "choose" is whatever their caller chose. Excluded by PATH rather than by
# "the argument is a variable", because the latter is a loophole -- assigning
# AXL_JSON_RELAXED to a local first would dodge the gate from anywhere.
EXCLUDE_PREFIXES = ("src/data/axl-json-",)

# A gate that inspects nothing reports clean forever. This is the count of real
# call sites the scan must still find; drop below it and the matcher is broken,
# not the tree. Deliberately well under the current total so ordinary
# refactoring does not trip it.
MIN_SITES = 4


def skip_literal(s: str, i: int) -> int:
    """@a i points at a quote; return the index just past the literal."""
    quote = s[i]
    i += 1
    while i < len(s):
        if s[i] == "\\":
            i += 2
            continue
        if s[i] == quote:
            return i + 1
        i += 1
    return i


def split_args(s: str, start: int) -> list[str] | None:
    """@a start points just past the '('. Return the top-level argument texts.

    Brace/bracket/paren aware and literal aware, so a comma inside a string or
    a nested call does not split an argument.
    """
    depth = 0
    i = start
    begin = start
    out: list[str] = []
    while i < len(s):
        c = s[i]
        if c in "\"'":
            i = skip_literal(s, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if c == ")" and depth == 0:
                out.append(s[begin:i])
                return out
            depth -= 1
        elif c == "," and depth == 0:
            out.append(s[begin:i])
            begin = i + 1
        i += 1
    return None


def is_definition(text: str, call_start: int) -> bool:
    """True when this occurrence is the function's own definition/declaration.

    The house style puts the return type on its own line above the name, so a
    definition is recognisable by the name sitting at column 0.
    """
    line_start = text.rfind("\n", 0, call_start) + 1
    return line_start == call_start


# How far above a call the marker may sit. A justification worth writing runs
# to several lines, and the window has to hold the whole comment block rather
# than only its first line -- a marker that silently falls out of range reads
# as "someone forgot to justify this", which is the opposite of the truth.
MARKER_WINDOW_LINES = 10


def preceding_lines(text: str, call_start: int,
                    n: int = MARKER_WINDOW_LINES) -> str:
    """The @a n source lines immediately before the call, for marker lookup."""
    end = text.rfind("\n", 0, call_start)
    if end < 0:
        return ""
    start = end
    for _ in range(n):
        prev = text.rfind("\n", 0, start)
        if prev < 0:
            start = 0
            break
        start = prev
    return text[start:end]


def check_file(path: Path) -> tuple[list[str], int]:
    text = path.read_text()
    rel = path.relative_to(ROOT)
    problems: list[str] = []
    seen = 0

    for name, arg_index in ENTRIES.items():
        pos = 0
        while True:
            idx = text.find(name + "(", pos)
            if idx < 0:
                break
            before = text[idx - 1] if idx > 0 else " "
            if before.isalnum() or before == "_":
                pos = idx + 1
                continue
            if is_definition(text, idx):
                pos = idx + 1
                continue
            args = split_args(text, idx + len(name) + 1)
            if args is None or len(args) <= arg_index:
                pos = idx + 1
                continue
            pos = idx + len(name)
            seen += 1

            dialect = " ".join(args[arg_index].split())
            if dialect == REQUIRED:
                continue
            if MARKER in preceding_lines(text, idx):
                continue
            line = text.count("\n", 0, idx) + 1
            problems.append(
                f"{rel}:{line}: {name}(...) parses with `{dialect}`, not "
                f"{REQUIRED}"
            )
    return problems, seen


def main() -> int:
    problems: list[str] = []
    total = 0
    files = 0
    for d in SCAN_DIRS:
        for path in sorted((ROOT / d).rglob("*.c")):
            rel = path.relative_to(ROOT).as_posix()
            if rel.startswith(EXCLUDE_PREFIXES):
                continue
            files += 1
            found, seen = check_file(path)
            problems.extend(found)
            total += seen

    if total < MIN_SITES:
        print(f"check-json-dialect: FAIL — found only {total} call site(s) "
              f"across {files} file(s) in {'/, '.join(SCAN_DIRS)}/.")
        print("  The matcher is broken. A gate that inspects nothing reports")
        print("  clean forever, so too few sites is a failure, not a pass.")
        return 1

    if problems:
        print("check-json-dialect: FAIL — JSON parsed with a non-strict "
              "dialect:")
        for p in problems:
            print(f"    {p}")
        print()
        print("  Design decision 40: anything crossing the network is strict")
        print("  RFC 8259, because a peer that sends JSON5 is broken or")
        print("  probing. If this really does read a LOCAL file, say so on the")
        print("  line above:")
        print(f"      /* {MARKER} -- <why> */")
        return 1

    print(f"check-json-dialect: clean — {total} JSON parse site(s) in "
          f"{'/, '.join(SCAN_DIRS)}/ are strict or marked local.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
