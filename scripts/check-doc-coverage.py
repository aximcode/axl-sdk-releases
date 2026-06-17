#!/usr/bin/env python3
"""check-doc-coverage.py — flag public headers missing from the Sphinx docs.

Every public header (`include/axl/*.h`) should have its API rendered in the
generated reference, which means it must be named in a `.. doxygenfile:: <h>`
directive somewhere under `docs/sphinx/`. When a new header lands without that
wiring, its whole API silently vanishes from the docs — exactly the drift this
gate prevents.

What it CANNOT catch: prose staleness — a module README that *describes* the old
behaviour after the code changed (e.g. "this does not move bytes" once byte I/O
is added). That is a workflow concern, not a structural one; see CLAUDE.md
"Documentation".

Two escape hatches:
  - EXEMPT: headers that legitimately get no API-reference page (the umbrella,
    pure-macro/entry glue, version/type plumbing).
  - TODO: a tracked backlog of real API not yet wired. Listed here so the gate
    passes today while the debt stays visible; wiring one into Sphinx (adding
    its doxygenfile directive) makes this script nudge you to drop it from TODO.

A header that is in NEITHER set and NOT referenced fails the gate — so any *new*
public header must be consciously wired or exempted. Exit status is non-zero on
a failure, so it can gate CI. Usage: scripts/check-doc-coverage.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER_DIR = ROOT / "include" / "axl"
SPHINX_DIR = ROOT / "docs" / "sphinx"

# Headers that legitimately have no API-reference page.
EXEMPT: set[str] = {
    "axl.h",            # umbrella — includes every other public header
    "axl-macros.h",     # AXL_APP / AXL_DRIVER entry glue + helper macros
    "axl-types.h",      # foundational typedefs, pulled in everywhere
    "axl-version.h",    # generated version constants
    "axl-mem-impl.h",   # inline allocator impl detail behind axl-mem.h
}

# Tracked backlog: real public API not yet in the Sphinx reference. Pre-existing
# at the time this gate was added; NOT a license to add more. Wire one in (give
# it a `.. doxygenfile::` directive) and drop it from this set.
TODO: set[str] = {
    "axl-ata.h",
    "axl-scsi.h",
    "axl-smart.h",
    "axl-gfx.h",
    "axl-jose.h",
    "axl-crashrecord.h",
}


def referenced_headers() -> set[str]:
    """Headers named in any `.. doxygenfile:: <name>.h` directive."""
    refs: set[str] = set()
    pattern = re.compile(r"doxygenfile::\s*(\S+\.h)")
    for rst in SPHINX_DIR.rglob("*.rst"):
        for match in pattern.finditer(rst.read_text(encoding="utf-8")):
            refs.add(Path(match.group(1)).name)
    return refs


def main() -> int:
    refs = referenced_headers()
    headers = sorted(p.name for p in HEADER_DIR.glob("*.h"))

    missing = [
        h for h in headers
        if h not in EXEMPT and h not in TODO and h not in refs
    ]
    if missing:
        print("check-doc-coverage: public headers with NO Sphinx doxygenfile "
              "directive (their API is missing from the docs):")
        for h in missing:
            print(f"  - include/axl/{h}")
        print()
        print(f"Fix: add `.. doxygenfile:: {missing[0]}` to the right "
              "docs/sphinx/modules/*.rst (a new standalone type may want its "
              "own page + an index.rst entry), and update that module's README "
              "prose. If it genuinely needs no API page, add it to EXEMPT in "
              "scripts/check-doc-coverage.py.")
        return 1

    # Nudge: a TODO header that has since been wired should leave the backlog.
    cleared = sorted(h for h in TODO if h in refs)
    if cleared:
        print("check-doc-coverage: these headers are now documented — remove "
              "them from TODO in scripts/check-doc-coverage.py: "
              + ", ".join(cleared))
        return 1

    remaining = sorted(h for h in TODO if h not in refs)
    if remaining:
        print(f"check-doc-coverage: clean ({len(remaining)} known-undocumented "
              f"headers tracked as TODO: {', '.join(remaining)}).")
    else:
        print("check-doc-coverage: clean — every public header is in the "
              "Sphinx reference.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
