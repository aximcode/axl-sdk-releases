#!/usr/bin/env python3
"""check-clang-warnings.py — compile every TU with clang and fail on a warning.

The tree builds with gcc; `scripts/lint.sh` runs clang-*tidy*. Nobody ran the
clang *compiler*, and clang-tidy does not close that gap: tidy reports the
checks named in .clang-tidy, not the frontend's own -W diagnostics. So a
clang-only compiler warning had nowhere to surface -- a `-Wformat` "zero field
width" sat in a committed test until it was noticed by hand, and any consumer
building the SDK with clang would meet the same class of diagnostic first.

This runs `clang -fsyntax-only` over every C translation unit in
compile_commands.json, with the REAL build flags (same -I/-D/-std the object was
compiled with), and fails if clang emits anything. No objects are produced, so
it is fast and cannot disturb the build tree.

Scope, and why (measured on 341 project TUs before this gate landed):

  -Wall                   2 findings, both real; fixed in the landing commit.
  -Wextra                 +73, of which 60 are -Wmissing-field-initializers.

-Wmissing-field-initializers is DISABLED, deliberately. In C, a partial
initializer zero-fills the rest -- that is the language guarantee, and
`AxlFoo f = { .a = 1 };` relying on it is a correct, pervasive idiom here (60
sites across src/, test/ and tools/). The check exists for C++ ctor semantics
and finds no real defect in this tree; "fixing" it would mean writing out
dozens of explicit zeros. This is the one suppression, and it is not a category
that has ever found a bug here.

Everything else -Wextra brings IS kept: -Wsign-compare and -Wunused-parameter
had 4 and 7 live instances respectively, all fixed rather than silenced.

Vendored code (deps/) is excluded as a TU and filtered from the output, since a
warning inside deps/libvterm or deps/sdefl is not ours to fix and would make the
gate permanently red.

Usage: scripts/check-clang-warnings.py [-p BUILD_DIR]
  Requires a current compile_commands.json (scripts/lint.sh generates one via
  bear; that is where this check is wired in).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def cross_libc_include() -> str:
    """The cross toolchain's libc include directory, from the Makefile.

    This gate REPLAYS the compile database with a different driver, and the
    libc headers are IMPLICIT in the recorded compiler binary -- nothing on the
    command line names <toolchain>/x86_64-elf/include. Left alone, clang
    resolves <string.h> to the HOST's /usr/include, so this measured a
    glibc-flavoured version of a freestanding program: clean on a
    non-multiarch distro, and wrong everywhere. clang-tidy hit the same rock
    and died outright in CI; this one stayed quiet, which is worse.

    The Makefile owns the value (it asks $(CC), so a toolchain override moves
    it too) and fails rather than printing an empty string.
    """
    proc = subprocess.run(["make", "-s", "print-cc-libc-include"],
                          cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout.strip():
        sys.exit("check-clang-warnings: cannot resolve the cross libc include "
                 f"directory:\n{proc.stderr.strip()}")
    return proc.stdout.strip()

# Added on top of the recorded build flags.
EXTRA_FLAGS = [
    "-fsyntax-only",
    "-Wall",
    "-Wextra",
    # See the module docstring: C's zero-fill guarantee makes this a style
    # opinion, not a defect signal, and it is 60 of the 75 -Wextra findings.
    "-Wno-missing-field-initializers",
    # clang does not consume every gcc flag the build passes (-mno-red-zone in
    # a syntax-only run, -ffunction-sections with no codegen, ...). That is
    # expected and says nothing about the source.
    "-Wno-unused-command-line-argument",
]

# Recorded flags that make no sense for, or actively break, a syntax-only run.
DROP_EXACT = {"-c", "-MD", "-MP"}
DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}

# TUs we do not own or cannot compile the same way.
def excluded(path: str) -> bool:
    return (
        not path.endswith(".c")
        or "/deps/" in path
        or path.startswith("deps/")
        or "mbedtls-platform" in path
    )


def clang_command(entry: dict[str, object], libc_include: str) -> list[str] | None:
    args = entry.get("arguments")
    if not isinstance(args, list):
        return None
    out: list[str] = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in DROP_WITH_ARG:
            skip = True
            continue
        if arg in DROP_EXACT:
            continue
        out.append(str(arg))
    # A cross-compiled TU records a compiler whose NAME is the only place the
    # target appears (x86_64-elf-gcc / aarch64-none-elf-gcc) -- replacing
    # argv[0] with plain `clang` silently retargeted it at the host. Name the
    # triple, then supply the libc that goes with it: compiler headers first
    # (-nostdlibinc keeps clang's builtins and drops /usr/include), the cross
    # libc after, which is the order the real build searches.
    #
    # NOT every entry is cross-compiled: scripts/pe-set-debug.c is a HOST tool
    # built with plain `gcc`, and host headers are correct for it. Retargeting
    # it broke the gate outright ("unknown target triple 'gcc'"), which is the
    # good failure -- silently handing it a freestanding libc would not have
    # announced itself.
    name = os.path.basename(out[0])
    triple = name[: -len("-gcc")] if name.endswith("-gcc") and name != "gcc" else ""
    out[0] = os.environ.get("CLANG", "clang")
    cross: list[str] = []
    if triple:
        cross = [f"--target={triple}", "-nostdlibinc", f"-idirafter{libc_include}"]
    return out + cross + EXTRA_FLAGS


def run_one(cmd: list[str], cwd: Path) -> str:
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return proc.stderr


def ours(line: str) -> bool:
    """Keep diagnostics whose file is ours; drop vendored headers."""
    return "/deps/" not in line and not line.startswith("deps/")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--build-dir", default=str(ROOT),
                        help="directory holding compile_commands.json")
    opts = parser.parse_args()

    db_path = Path(opts.build_dir) / "compile_commands.json"
    if not db_path.is_file():
        print(f"check-clang-warnings: no compile_commands.json at {db_path}\n"
              "  Generate one first:  bear -- make tests tools\n"
              "  (scripts/lint.sh does this and then runs this check.)",
              file=sys.stderr)
        return 2

    entries = json.loads(db_path.read_text())
    libc_include = cross_libc_include()
    jobs: list[tuple[str, list[str], Path]] = []
    seen: set[str] = set()
    for entry in entries:
        path = str(entry.get("file", ""))
        if path in seen or excluded(path):
            continue
        seen.add(path)
        cmd = clang_command(entry, libc_include)
        if cmd is None:
            continue
        # The database is single-arch (one `make` invocation produced it) while
        # the libc directory above came from make's DEFAULT arch. Linting an
        # ARCH=aa64 database would otherwise analyze aarch64 TUs against x64
        # newlib headers and report something meaningless about neither.
        for arg in cmd:
            if arg.startswith("--target=") and arg[len("--target="):] not in libc_include:
                sys.exit(
                    f"check-clang-warnings: {path} targets "
                    f"{arg[len('--target='):]}, but the libc headers reported "
                    f"by make are {libc_include}.\n  Regenerate "
                    "compile_commands.json for the arch you are linting, or "
                    "run make with the matching ARCH."
                )
        jobs.append((path, cmd, Path(str(entry.get("directory", ROOT)))))

    if not jobs:
        print("check-clang-warnings: no translation units found — is the "
              "compile database stale?", file=sys.stderr)
        return 2

    findings: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        futures = {ex.submit(run_one, cmd, cwd): path
                   for path, cmd, cwd in jobs}
        for future in concurrent.futures.as_completed(futures):
            for line in future.result().splitlines():
                if ("warning:" in line or "error:" in line) and ours(line):
                    findings.append(line)

    if findings:
        print(f"check-clang-warnings: FAIL — clang reported "
              f"{len(findings)} diagnostic(s):")
        for line in sorted(findings):
            print(f"  {line}")
        print("\n  The tree builds with gcc, so these never surfaced. Fix them,"
              "\n  or (for a whole category that is noise in C) state the reason"
              "\n  in EXTRA_FLAGS in scripts/check-clang-warnings.py.")
        return 1

    print(f"check-clang-warnings: clean ({len(jobs)} translation units, "
          f"clang -Wall -Wextra).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
