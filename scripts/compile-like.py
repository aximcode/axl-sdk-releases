#!/usr/bin/env python3
"""compile-like.py -- compile a source exactly as the build compiles another.

The compile-only gates (check-cxx-entry, check-cb-noexcept,
check-handle-exclusions, check-examples) compile throwaway fixtures with the
tree's OWN flags. The retired Makefile handed them `$(CXX) $(CXXFLAGS_BASE)`;
the CMake build's flags live in generator expressions on interface targets, so
the one place they exist as a command line is compile_commands.json. This
takes the command the build runs for a LIKE translation unit (an application
TU of the right language), swaps in the source and the output, optionally
drops and adds flags, and runs it from the tree's directory as the build
does.

Usage:
  compile-like.py --tree DIR --like SRC --src FILE -o OUT [--drop FLAG]...
                  [--add FLAG]... [--print]

  --like   a path suffix naming the like TU (`sdk/examples/hello.c`); exactly
           one compile_commands.json entry must end with it
  --drop   remove every word equal to FLAG (e.g. -DAXL_ALLOW_UEFI); a FLAG
           with spaces removes that run of words (`-isystem DIR`). A drop
           that matches NOTHING is a failure (125), never a silent no-op
  --add    append FLAG before the source
  --print  print the command (one word per line) and compile nothing

Exit: the compiler's own status (its output passes through); 125 when the
command cannot be had -- no database, no entry, two entries, no -o/-c in it,
a bad argument. (125, not 2: a compiler's own failure codes are small, and
the caller must tell "could not compile" from "could not even ask".)
Pass flags that start with '-' as --add=FLAG / --drop=FLAG.
A gate that cannot build its command must not read as a compile that failed,
nor as one that passed.
"""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

CANNOT = 125


class Parser(argparse.ArgumentParser):
    """argparse that exits CANNOT on a usage error, not its own 2."""

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        print(f"compile-like: {message}", file=sys.stderr)
        sys.exit(CANNOT)


def fail(msg: str) -> int:
    print(f"compile-like: {msg}", file=sys.stderr)
    return CANNOT


def main(argv: list[str]) -> int:
    ap = Parser(prog="compile-like.py")
    ap.add_argument("--tree", required=True, type=Path)
    ap.add_argument("--like", required=True)
    ap.add_argument("--src", required=True, type=Path)
    ap.add_argument("-o", dest="out", required=True, type=Path)
    ap.add_argument("--drop", action="append", default=[])
    ap.add_argument("--add", action="append", default=[])
    ap.add_argument("--print", action="store_true")
    args = ap.parse_args(argv[1:])

    db = args.tree / "compile_commands.json"
    try:
        entries: list[dict[str, str]] = json.loads(db.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return fail(f"cannot read {db}: {exc}")
    like = "/" + args.like.lstrip("/")
    hits = [e for e in entries if e.get("file", "").endswith(like)]
    if len(hits) != 1:
        return fail(f"{len(hits)} entries in {db} end with {like} (want exactly one)")
    entry = hits[0]
    words = shlex.split(entry["command"])
    try:
        o_at = words.index("-o")
        c_at = words.index("-c")
    except ValueError:
        return fail(f"the command for {like} has no -o/-c: {entry['command']}")
    words[o_at + 1] = str(args.out.resolve())
    words[c_at + 1] = str(args.src.resolve())
    out = words
    for d in args.drop:
        seq = d.split(" ")
        n = len(seq)
        kept: list[str] = []
        i = 0
        while i < len(out):
            if out[i:i + n] == seq:
                i += n
                continue
            kept.append(out[i])
            i += 1
        if len(kept) == len(out):
            # A drop that matched nothing changed nothing: the flag it was
            # meant to remove is still there, or spelled another way (a
            # symlinked checkout, `-isystemDIR` as one word) -- and the
            # caller's whole point was its absence.
            return fail(f"--drop {d!r}: no such word (run) in the command for {like}")
        out = kept
    if args.add:
        c_now = out.index("-c")
        out[c_now:c_now] = args.add
    if args.print:
        print("\n".join(out))
        return 0
    return subprocess.run(out, cwd=entry.get("directory", str(args.tree)),
                          check=False).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
