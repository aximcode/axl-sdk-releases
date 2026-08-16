#!/usr/bin/env python3
"""lint-cache.py -- skip clang-tidy on translation units that cannot have changed.

    lint-cache.py filter <cache-dir> <salt-file> <tu>...   # prints TUs to lint
    lint-cache.py record <cache-dir> <salt-file> <tu>...   # marks them clean

clang-tidy over src/ is ~45s and is the largest item in a warm verify.sh, and
almost all of it re-analyses files that are byte-for-byte what they were last
time. A TU's result is a pure function of its own text, the text of every
header it includes, the flags it is compiled with, and the checker itself, so
caching on exactly that tuple is sound.

THE BAR IS NOT SPEED, IT IS THAT A SKIP IS NEVER WRONG. A cache that misses a
finding is a gate that cannot see, which this tree treats as worse than no gate
at all. So:

  - The key covers the TU and EVERY header in its .d dependency list, by
    CONTENT (sha256), not mtime. A `git checkout` that rewrites timestamps
    without changing bytes correctly stays cached; an edit that restores a file
    to a previously-linted state correctly re-uses that state.
  - The salt (a separate file the caller builds) covers the clang-tidy binary's
    version, the .clang-tidy config, the check-set argument, the extra clang
    args, and a hash of the whole compile database -- so a flag change anywhere
    invalidates everything rather than being invisible.
  - A TU with NO dependency record is always linted. Unknown means unsafe, and
    the failure mode of guessing is silence.
  - Nothing is recorded unless the caller says the pass SUCCEEDED. clang-tidy
    is run over a batch and any finding fails the batch, so a failed batch
    records nothing and the next run re-lints all of it.

Escape hatch: LINT_NO_CACHE=1 makes `filter` return everything. CI has no cache
directory to begin with, so it always does the full run.

The dependency lists come from the .d files the lint build already emits
(-MD -MP). They are read from the build tree named by $LINT_DEP_DIR.
"""

from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def _file_hashes(paths: set[Path]) -> dict[Path, str]:
    """sha256 per file, computed once for the union — headers are shared by
    many TUs, so hashing per-TU would read the same bytes dozens of times."""
    out: dict[Path, str] = {}
    for p in paths:
        try:
            out[p] = hashlib.sha256(p.read_bytes()).hexdigest()
        except OSError:
            out[p] = "MISSING"      # a vanished header must not hash stable
    return out


def _dep_map(dep_dir: Path) -> dict[str, list[str]]:
    """source path -> [its prerequisites], parsed from the build's .d files.

    Keyed on the SOURCE (the first prerequisite) rather than the .d filename,
    because .d files are named after the object and two directories can hold
    the same basename.
    """
    deps: dict[str, list[str]] = {}
    if not dep_dir.is_dir():
        return deps
    for d in dep_dir.glob("*.d"):
        try:
            text = d.read_text(errors="replace")
        except OSError:
            continue
        text = text.replace("\\\n", " ")
        if ":" not in text:
            continue
        # Only the first rule; -MP adds bare phony targets afterwards.
        rule = text.split("\n", 1)[0] if "\n" in text else text
        _, _, rhs = rule.partition(":")
        items = [x for x in rhs.split() if x and x != "\\"]
        if not items:
            continue
        src = os.path.normpath(items[0])
        deps[src] = [os.path.normpath(x) for x in items]
    return deps


def _keys(tus: list[str], salt: str, dep_dir: Path) -> dict[str, str | None]:
    """TU -> cache key, or None when it must be linted regardless."""
    deps = _dep_map(dep_dir)
    wanted: set[Path] = set()
    for tu in tus:
        for f in deps.get(os.path.normpath(tu), []):
            wanted.add(Path(f) if os.path.isabs(f) else REPO / f)
    hashes = _file_hashes(wanted)

    keys: dict[str, str | None] = {}
    for tu in tus:
        rec = deps.get(os.path.normpath(tu))
        if not rec:
            keys[tu] = None            # no dependency record -> always lint
            continue
        h = hashlib.sha256()
        h.update(salt.encode())
        for f in sorted(rec):
            p = Path(f) if os.path.isabs(f) else REPO / f
            h.update(f.encode())
            h.update(hashes.get(p, "MISSING").encode())
        keys[tu] = h.hexdigest()
    return keys


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    mode, cache_dir, salt_file = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    tus = sys.argv[4:]
    if not tus:
        return 0

    if mode == "filter" and os.environ.get("LINT_NO_CACHE") == "1":
        print("\n".join(tus))
        return 0

    salt = salt_file.read_text() if salt_file.is_file() else ""
    dep_dir = Path(os.environ.get("LINT_DEP_DIR", "out/native-x64-lint/build"))
    if not dep_dir.is_absolute():
        dep_dir = REPO / dep_dir

    keys = _keys(tus, salt, dep_dir)

    if mode == "filter":
        need = [tu for tu in tus
                if keys[tu] is None or not (cache_dir / keys[tu]).exists()]
        if need:
            print("\n".join(need))
        return 0

    if mode == "record":
        cache_dir.mkdir(parents=True, exist_ok=True)
        for tu in tus:
            k = keys[tu]
            if k:
                (cache_dir / k).touch()
        return 0

    print(f"unknown mode '{mode}'", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
