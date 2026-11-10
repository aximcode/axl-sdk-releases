#!/usr/bin/env python3
"""compare-trees.py -- do two staged trees (or archives) hold the same files?

Usage: scripts/compare-trees.py [--layout-only] [--strip N] A B

A and B are each a directory or a .tar.gz/.tgz/.tar archive. Compared:
every path's TYPE (file, dir, symlink), its MODE (permission bits), a
symlink's TARGET and a regular file's BYTES -- never mtimes or owners, which
a staging path legitimately sets differently (the reproducibility of an
archive is a separate, byte-level question its own test asks).

--strip N drops the first N components of every archive member path, as
`tar --strip-components` does, so an archive whose versioned root differs
from a staged prefix's directory name compares by what is under it.
--layout-only skips the byte comparison: names, types, modes and link
targets only (for two archives built from different sources, where only the
layout is meant to agree).

Every difference is printed, one line each, sorted; exit 1 if there is any,
0 if none, 2 on a usage error or an input that cannot be read. An EMPTY
comparison (neither side has an entry) is refused with exit 2: two empty
trees agree, and a comparison that saw nothing has shown nothing.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import stat
import sys
import tarfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Entry:
    """One path's comparable facts."""

    kind: str          # "file", "dir", "link"
    mode: int          # permission bits only
    target: str        # a symlink's target, else ""
    digest: str        # a regular file's SHA-256, else ""


def hash_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _raise(err: OSError) -> None:
    """os.walk's error hook: an unreadable directory is a failure, never an
    empty one -- two unreadable sides would otherwise compare identical."""
    raise err


def read_dir(root: Path) -> dict[str, Entry]:
    out: dict[str, Entry] = {}
    for dirpath, dirnames, filenames in os.walk(root, onerror=_raise):
        base = Path(dirpath)
        for name in sorted(dirnames) + sorted(filenames):
            p = base / name
            rel = p.relative_to(root).as_posix()
            st = p.lstat()
            mode = stat.S_IMODE(st.st_mode)
            if stat.S_ISLNK(st.st_mode):
                out[rel] = Entry("link", 0, os.readlink(p), "")
            elif stat.S_ISDIR(st.st_mode):
                out[rel] = Entry("dir", mode, "", "")
            elif stat.S_ISREG(st.st_mode):
                out[rel] = Entry("file", mode, "", hash_bytes(p.read_bytes()))
            else:
                out[rel] = Entry("other", mode, "", "")
    return out


def strip_path(name: str, strip: int) -> str | None:
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if len(parts) <= strip:
        return None
    return "/".join(parts[strip:])


def read_tar(path: Path, strip: int) -> dict[str, Entry]:
    out: dict[str, Entry] = {}
    with tarfile.open(path, "r:*") as tf:
        for m in tf.getmembers():
            rel = strip_path(m.name, strip)
            if rel is None:
                continue
            mode = m.mode & 0o7777
            if m.issym():
                out[rel] = Entry("link", 0, m.linkname, "")
            elif m.isdir():
                out[rel] = Entry("dir", mode, "", "")
            elif m.isfile():
                f = tf.extractfile(m)
                data = f.read() if f is not None else b""
                out[rel] = Entry("file", mode, "", hash_bytes(data))
            else:
                out[rel] = Entry("other", mode, "", "")
    return out


def load(arg: str, strip: int) -> dict[str, Entry]:
    p = Path(arg)
    if p.is_dir():
        return read_dir(p)
    if p.is_file() and (arg.endswith((".tar.gz", ".tgz", ".tar"))):
        return read_tar(p, strip)
    raise OSError(f"not a directory or a .tar[.gz] archive: {arg}")


def compare(a: dict[str, Entry], b: dict[str, Entry],
            contents: bool) -> list[str]:
    diffs: list[str] = []
    for rel in sorted(set(a) | set(b)):
        ea, eb = a.get(rel), b.get(rel)
        if ea is None:
            diffs.append(f"only in B: {rel}")
            continue
        if eb is None:
            diffs.append(f"only in A: {rel}")
            continue
        if ea.kind != eb.kind:
            diffs.append(f"type differs: {rel} ({ea.kind} vs {eb.kind})")
            continue
        if ea.mode != eb.mode:
            diffs.append(f"mode differs: {rel} ({ea.mode:o} vs {eb.mode:o})")
        if ea.target != eb.target:
            diffs.append(f"link target differs: {rel} ({ea.target} vs {eb.target})")
        if contents and ea.digest != eb.digest:
            diffs.append(f"bytes differ: {rel}")
    return diffs


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--layout-only", action="store_true",
                    help="names, types, modes and link targets; not bytes")
    ap.add_argument("--strip", type=int, default=0,
                    help="leading path components to drop from archive members")
    ap.add_argument("a")
    ap.add_argument("b")
    args = ap.parse_args(argv)
    try:
        a = load(args.a, args.strip)
        b = load(args.b, args.strip)
    except (OSError, tarfile.TarError) as e:
        print(f"compare-trees: {e}", file=sys.stderr)
        return 2
    if not a and not b:
        print("compare-trees: both sides are empty -- nothing was compared",
              file=sys.stderr)
        return 2
    diffs = compare(a, b, not args.layout_only)
    for d in diffs:
        print(d)
    if diffs:
        print(f"compare-trees: {len(diffs)} difference(s) over "
              f"{len(set(a) | set(b))} path(s)", file=sys.stderr)
        return 1
    print(f"compare-trees: identical ({len(a)} path(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
