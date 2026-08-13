#!/usr/bin/env python3
"""check-nul.py — fail if a tracked text file contains a literal NUL byte.

Writing an escape sequence like a backslash-u-0000 or backslash-x-00 through an
editing tool can insert a REAL NUL byte instead of the two/four characters that
were meant. The result is a `.c` or `.md` that git now classifies as binary: no
diff is shown on review, `grep` skips it, and the compiler may or may not
complain depending on where the byte landed. Nothing in the build noticed --
each occurrence was caught by chance.

This is the cheap structural guard. A tracked file whose extension is not on the
BINARY_EXT denylist is expected to be text, so a NUL in it is always a defect.
Reports the file, byte offset, and 1-based line, so the fix is a direct seek.

Deliberately denylist-based rather than allowlist-based: a *new* kind of text
file (.toml, .cfg, some extensionless script) is then covered automatically,
which is the direction the failure mode actually travels. A new BINARY kind has
to be added below -- a one-line, obvious change when it happens.

Usage: scripts/check-nul.py   (run from anywhere; resolves the repo itself)
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Tracked extensions whose contents are legitimately binary. Everything else
# tracked is treated as text. Keep lowercase; comparison is case-insensitive.
BINARY_EXT: set[str] = {
    ".efi", ".bin", ".ttf", ".png", ".jpg", ".jpeg", ".gif", ".ico",
    ".pdf", ".gz", ".xz", ".bz2", ".zip", ".tar",
    ".o", ".a", ".so", ".obj", ".lib", ".exe", ".dll",
    ".fd", ".rom", ".img", ".fv", ".cap",
    ".woff", ".woff2", ".pyc",
}


def tracked_files() -> list[Path]:
    """Every path in the index, NUL-separated so odd filenames survive."""
    # stdout only: git's stderr must reach the console. Capturing it meant a
    # failure here raised CalledProcessError with the diagnosis sealed inside
    # the exception object, so CI printed a traceback naming the command and
    # the exit code but not the reason -- which is how "detected dubious
    # ownership" cost a container reproduction to read.
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT, check=True, stdout=subprocess.PIPE,
    ).stdout
    return [ROOT / name.decode() for name in out.split(b"\0") if name]


def main() -> int:
    bad: list[str] = []
    scanned = 0
    absent = 0

    for path in tracked_files():
        if path.suffix.lower() in BINARY_EXT:
            continue
        # Submodule gitlinks appear in ls-files as directories; symlinks may
        # dangle. Neither is a text file we can or should read.
        if not path.is_file():
            # Not there AT ALL (not a dir, not a dangling symlink) means git
            # listed a path the working tree does not have -- a sparse or
            # partial checkout. Counted, and judged after the loop.
            if not path.is_dir() and not path.is_symlink():
                absent += 1
            continue
        data = path.read_bytes()
        scanned += 1
        off = data.find(b"\0")
        if off < 0:
            continue
        line = data.count(b"\n", 0, off) + 1
        rel = path.relative_to(ROOT)
        total = data.count(b"\0")
        bad.append(f"  {rel}: NUL byte at offset {off} (line {line})"
                   f"{f', {total} total' if total > 1 else ''}")

    if bad:
        print("check-nul: FAIL — literal NUL byte in tracked text file(s):")
        print("\n".join(bad))
        print("\n  A NUL turns the file binary: git shows no diff, grep skips "
              "it.\n  Usually a backslash-u-0000 / backslash-x-00 escape that "
              "an editing tool\n  wrote as the byte it denotes instead of as "
              "source text. Re-write the\n  escape, or (for a deliberate "
              "binary fixture) add its extension to\n  BINARY_EXT in "
              "scripts/check-nul.py.")
        return 1

    # Judge on what was actually READ versus what the index says should be
    # there. `check=True` catches only a non-zero exit, and git exits 0 for a
    # sparse checkout -- it still LISTS every excluded path, so the paths
    # survive and are dropped one at a time by the is_file() skip above. The
    # gate would then print "clean (N tracked text files)" for a tiny N and
    # exit 0, passing precisely because it looked at almost nothing. Measured:
    # a sparse clone lists 1352 and has 0 on disk.
    #
    # The threshold is `absent > scanned` rather than `absent > 0` so that a
    # file deleted mid-edit but still staged does not fail an unrelated lint
    # gate. Wholesale absence is the failure worth catching; one or two
    # missing paths is a working tree in flux.
    # `scanned == 0` is its own case, not covered by the ratio: an EMPTY index
    # gives 0 and 0, and `0 > 0` is false, so the ratio alone would print
    # "clean (0 tracked text files)" and exit 0 -- the precise blindness this
    # block exists to stop.
    if scanned == 0 or absent > scanned:
        print(f"check-nul: FAIL — {absent} tracked file(s) are missing from "
              f"the working tree and only {scanned} were read.")
        print(f"  repo root: {ROOT}")
        print("  git lists these paths but they are not on disk — a sparse or"
              "\n  partial checkout. The gate cannot verify what it cannot "
              "read,\n  so it will not report clean.")
        return 1

    print(f"check-nul: clean ({scanned} tracked text files).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
