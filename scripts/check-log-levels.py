#!/usr/bin/env python3
"""check-log-levels.py -- axl_info in library code must justify itself.

THE RULE (docs/AXL-Coding-Style.md, "Log levels in library code"). A library
must not log above debug for a condition it reports to its caller through a
return value, and must never log above debug on a success path. `axl_info` in
particular must not announce a success, a readiness, or the completion of a
step that returns a status -- the caller learns the same fact from a non-NULL
handle or an AXL_OK, and only the caller has the context to judge whether it
matters.

WHY A GATE. This defect has now shipped twice. v3.2.1 demoted the eight sites a
consumer had actually tripped over; the consumer deleted its domain-pinning
workaround, re-measured, and still saw six lines -- because the original list
was assembled from observed output rather than from a census. A rule that lives
only in a document is re-broken by the next author who has not read it, and a
list of known-bad sites cannot catch the site added tomorrow.

WHY A MARKER RATHER THAN A KEYWORD SCAN. The obvious cheap version greps
axl_info strings for ready|installed|listening|loaded|initialized|detected.
That rots on contact: it passes "SMBus transport is up", "network came alive",
or any phrasing nobody thought of, while the sites it does catch are exactly
the ones a reviewer would have caught anyway. Requiring a marker inverts the
burden -- INFO becomes a level you opt into and defend, which is what it should
have been -- and it has neither false positives nor false negatives, because it
asks a question about intent instead of guessing from vocabulary. It is also
the shape this tree already uses for a deliberate exception; see
check-json-dialect.py's argument for markers over allow-lists.

THE MARKER. On the line above the call (or at the end of the call's own first
line):

    /* log-level: <why this is not a success announcement> */
    axl_info("...");

There is exactly ONE in the tree, and it is worth reading as the worked
example, because it is the case the rule's plain wording would get wrong:

    src/mem/axl-mem.c -- axl_mem_dump_leaks() returns void and its entire
    PURPOSE is to report state. The log is not a side-channel commentary on
    an operation that returned a status; it IS the return value. The QEMU
    harness's leak gate greps for that exact line, so demoting it does not
    make the tree quieter -- it makes the leak gate blind, which CLAUDE.md
    calls worse than having no gate at all.

That distinction -- reporting AS the contract, versus announcing that work
succeeded -- is the line the rule draws, and a marker is where an author
records which side of it they believe they are on.

NOT SCANNED. test/ and sdk/examples/ (an example that prints at INFO is
demonstrating the API, not being a library), and tools/ (a tool's output is
its product; it is the consumer, not the library).

Usage: scripts/check-log-levels.py   (run from anywhere)
Exit 0 clean, 1 on any unjustified call.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Both comment styles: the tree uses `//` at least as much as `/* */`, and a
# marker that only one of them can satisfy is a marker most authors cannot use.
MARKER = re.compile(r"(/\*|//)\s*log-level:")

# axl_info() is the macro, but it expands to axl_log_full(AXL_LOG_INFO, ...),
# and both that and axl_log(AXL_LOG_INFO, ...) are callable directly -- so
# matching only the macro leaves a way to emit INFO that the gate cannot see.
CALL = re.compile(r"\baxl_info\s*\(|\baxl_log(?:_full)?\s*\(\s*AXL_LOG_INFO\b")

REPO = Path(__file__).resolve().parent.parent
SCAN_ROOT = REPO / "src"

# .h too: a static inline in a header emits from wherever it is included, and
# .cpp because the C++ layer under src/runtime/ is library code like any other.
SCAN_SUFFIXES = ("*.c", "*.cpp", "*.h", "*.hpp")


def justified(lines: list[str], idx: int) -> bool:
    """True if the call at @idx carries a marker on its own line or anywhere in
    the comment immediately above it.

    Scanning the WHOLE preceding comment, not just one line, is load-bearing: a
    justification worth writing runs to several lines, and its first line --
    the one holding the marker -- is then furthest from the call. A one-line
    lookback silently rejects exactly the well-explained exceptions this exists
    to admit, and a rejected call gets demoted, so the gate ends up reporting
    "clean" over a tree it just emptied. Not hypothetical: it happened while
    writing this, to the one call the QEMU leak gate depends on.

    Note this walks to the opening `/*` rather than testing each line for a
    comment prefix. House style does not begin continuation lines with `*`, so
    a prefix test stops at the first line of prose and misses the marker above
    it -- which is the same bug a second time, from the other end.
    """
    if MARKER.search(lines[idx]):
        return True
    if idx == 0:
        return False

    above = lines[idx - 1].strip()

    if above.endswith("*/"):
        j = idx - 1
        while j >= 0:
            if MARKER.search(lines[j]):
                return True
            if "/*" in lines[j]:
                return False
            j -= 1
        return False

    if above.startswith("//"):
        j = idx - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            if MARKER.search(lines[j]):
                return True
            j -= 1
        return False

    return False


def violations() -> list[tuple[Path, int, str]]:
    """Every axl_info call under src/ that carries no justification marker."""
    found: list[tuple[Path, int, str]] = []

    paths: set[Path] = set()
    for suffix in SCAN_SUFFIXES:
        paths.update(SCAN_ROOT.rglob(suffix))

    for path in sorted(paths):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for idx, line in enumerate(lines):
            if not CALL.search(line) or justified(lines, idx):
                continue
            found.append((path.relative_to(REPO), idx + 1, line.strip()))

    return found


def main() -> int:
    bad = violations()
    if not bad:
        print("check-log-levels: clean (every axl_info under src/ is justified)")
        return 0

    print(f"check-log-levels: {len(bad)} unjustified axl_info call(s) under src/\n",
          file=sys.stderr)
    for path, line_no, text in bad:
        print(f"  {path}:{line_no}: {text[:96]}", file=sys.stderr)

    print(
        "\naxl_info must not announce a success, a readiness, or the completion\n"
        "of a step that returns a status -- the caller already learns that from\n"
        "the return value, and only the caller knows whether it matters. Demote\n"
        "to axl_debug; the line is still there under -v.\n"
        "\n"
        "If a call really does report something the caller asked for and cannot\n"
        "infer -- a void function whose PURPOSE is to report -- say so on the\n"
        "line above it:\n"
        "\n"
        "    /* log-level: <why this is not a success announcement> */\n"
        "\n"
        "See docs/AXL-Coding-Style.md and the docstring at the top of this file.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
