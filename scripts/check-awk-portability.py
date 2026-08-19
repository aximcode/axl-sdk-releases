#!/usr/bin/env python3
"""check-awk-portability.py — no gawk-only awk functions in the build.

WHY THIS EXISTS. `/usr/bin/awk` is **mawk** in every container this project
builds and lints in (verified in ubuntu:24.04 and ubuntu:26.04; neither
workflow's apt list installs gawk). A gawk extension therefore works on a
developer box and fails in CI -- and the failure is not always loud:

  - `MALLINFO_SIZE` used `strtonum` inside `$(shell ...)`. mawk exits 2 and
    prints nothing, so the variable expanded to EMPTY and
    `-DAXL_NEWLIB_MALLINFO_INT=1` was silently NOT applied in any CI build,
    while every local build applied it. A compile flag that differed between
    local and CI, undetected.
  - `check-cxx-entry` used `strtonum` in a recipe. There it fails loudly, but
    with the WRONG diagnosis ("registered NO .init_array entry"), which sends
    the reader after a nonexistent codegen bug.

Both are fixed by letting the shell do hex conversion (`$((0x$n))`, POSIX).
This gate keeps them fixed.

The list below is the gawk extensions plausible in build glue. It is
deliberately short: a name-based check with false positives would get
allowlisted into uselessness. `toupper`/`tolower`/`length`/`split`/`sub`/
`gsub`/`match`/`index`/`substr`/`sprintf`/`printf` are all POSIX and absent
from it.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# gawk-only functions. mawk has none of these.
GAWK_ONLY: tuple[str, ...] = (
    "strtonum",
    "gensub",
    "asort",
    "asorti",
    "systime",
    "strftime",
    "mktime",
    "patsplit",
    "typeof",
)

# Files whose awk runs on a DEVELOPER box only, never in a container build.
# Each entry needs a reason, and the reason has to be about where it RUNS.
EXEMPT: dict[str, str] = {}

CALL_RE = re.compile(r"\b(" + "|".join(GAWK_ONLY) + r")\s*\(")


def _shell_shebang(path: Path) -> bool:
    """True if the file starts with a shell #! line."""
    try:
        with path.open("rb") as fh:
            first = fh.readline(200)
    except OSError:
        return False
    return first.startswith(b"#!") and (b"sh" in first)


def tracked_build_files() -> list[Path]:
    """Makefile + the shell glue CI actually executes.

    Globs alone are not enough, and the gap was load-bearing: the two scripts
    that SHIP to consumers -- `scripts/axl-cc` and its `axl-c++` alias -- carry
    no extension, so `scripts/*.sh` never matched them. The driver holds the
    largest awk program in the tree and this gate could not see one byte of it;
    a planted `strtonum` in it was reported as "clean".

    So extensions find the obvious files and a shell shebang finds the rest.
    That is self-maintaining: the next extensionless script is covered on the
    day it is written, where a hardcoded name list would have to remember it.
    """
    out = subprocess.run(
        ["git", "ls-files", "-z", "Makefile", "*.mk", "scripts/*.sh",
         "test/integration/*.sh", "test/integration/lib/*.sh",
         ".github/workflows/*.yml"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout
    files = [ROOT / p for p in out.split("\0") if p]
    seen = {f.resolve() for f in files}

    extra = subprocess.run(
        ["git", "ls-files", "-z", "scripts/", "test/integration/",
         "test/integration/lib/"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout
    for rel in extra.split("\0"):
        if not rel:
            continue
        cand = ROOT / rel
        if cand.suffix or cand.resolve() in seen or not cand.is_file():
            continue
        if _shell_shebang(cand):
            files.append(cand)
            seen.add(cand.resolve())
    return files


def main() -> int:
    # `--list` prints what this gate would scan, one path per line. It exists
    # so coverage is ASSERTABLE: a scanner's silence means nothing until you
    # can show it was looking at the file. See test-awk-portability-gate.sh.
    if "--list" in sys.argv[1:]:
        for path in sorted(tracked_build_files()):
            print(path.relative_to(ROOT).as_posix())
        return 0

    findings: list[str] = []
    scanned = 0

    for path in tracked_build_files():
        rel = path.relative_to(ROOT).as_posix()
        if rel in EXEMPT:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        scanned += 1
        for lineno, line in enumerate(text.splitlines(), 1):
            m = CALL_RE.search(line)
            if m is not None:
                findings.append(f"  {rel}:{lineno}: {m.group(1)}() is gawk-only")
                findings.append(f"      {line.strip()[:100]}")

    if findings:
        print("check-awk-portability: FAIL — gawk-only awk function in build glue")
        print("  /usr/bin/awk is mawk in the ubuntu:24.04 / ubuntu:26.04 containers")
        print("  CI builds and lints in, and neither apt list installs gawk.")
        print()
        print("\n".join(findings))
        print()
        print("  For hex->decimal use the SHELL, which is POSIX:")
        print("      n=$(... | awk '/pat/ { print $2 }'); v=$((0x$n))")
        return 1

    print(f"check-awk-portability: clean — {scanned} build files, "
          f"no gawk-only functions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
