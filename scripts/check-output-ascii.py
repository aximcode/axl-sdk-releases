#!/usr/bin/env python3
"""check-output-ascii.py — flag non-ASCII bytes inside C/C++ string and char
literals.

A UEFI Simple Text Output console has only a limited (largely ASCII +
box-drawing) glyph set, so a Unicode em-dash / arrow / curly-quote / ellipsis
that reaches the console — via axl_print, axl_printf, the log helpers, or an
AxlArgs `.help` string — draws as a white block under a plain text console.
This checker finds such bytes in the *emittable* surface (string and char
literals) while ignoring comments, where the project freely uses Unicode.

It is intentionally conservative: it tokenizes C/C++ enough to tell a string
literal from a comment, then reports any literal byte > 0x7F. Genuinely
intentional Unicode (a UTF-8 / TTF rendering test, a deliberate non-ASCII
filename fixture) is exempted with an inline marker on the same line:

    { "résumé.txt", ... },   // ascii-allow: UCS-2/UTF-8 boundary fixture

Exit status is non-zero if any unexempted literal byte is found, so it can gate
CI. Usage:

    scripts/check-output-ascii.py [path ...]     # default: src tools sdk include
"""

from __future__ import annotations

import sys
from pathlib import Path

ALLOW_MARKER = "ascii-allow"
EXTS = {".c", ".h", ".cpp", ".cc", ".hpp"}

# ASCII replacements for decorative punctuation that a UEFI console can't draw.
# Anything non-ASCII NOT in this map is left untouched by --fix and reported,
# so a deliberate fixture is never silently mangled (mark it `ascii-allow`).
FIX_MAP = {
    "—": "-",     # em dash
    "–": "-",     # en dash
    "→": "->",    # rightwards arrow
    "←": "<-",    # leftwards arrow
    "…": "...",   # horizontal ellipsis
    "‘": "'", "’": "'",   # curly single quotes
}


def scan(text: str) -> list[tuple[int, str]]:
    """Return (line_no, codepoint-hex+char) for each non-ASCII byte that sits
    inside a string or char literal (comments and code are ignored)."""
    findings: list[tuple[int, str]] = []
    i, n = 0, len(text)
    line = 1
    # State: 'code' | 'line_comment' | 'block_comment' | 'string' | 'char'
    state = "code"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "\n":
            line += 1
            if state == "line_comment":
                state = "code"
            i += 1
            continue
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block_comment"
                i += 2
                continue
            if c == '"':
                state = "string"
                i += 1
                continue
            if c == "'":
                state = "char"
                i += 1
                continue
            i += 1
            continue
        if state == "line_comment":
            i += 1
            continue
        if state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
            i += 1
            continue
        if state in ("string", "char"):
            if c == "\\":             # escape: skip the next char verbatim
                i += 2
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
                i += 1
                continue
            if ord(c) > 0x7F:
                findings.append((line, f"U+{ord(c):04X} {c!r}"))
            i += 1
            continue
    return findings


def fix_text(text: str) -> tuple[str, int]:
    """Return (new_text, n_changes): replace mappable non-ASCII chars that sit
    inside a string/char literal with their ASCII equivalent. Lines carrying an
    `ascii-allow` marker, and any char not in FIX_MAP, are left untouched."""
    # Precompute which lines are exempt.
    allow_lines = {i + 1 for i, ln in enumerate(text.splitlines()) if ALLOW_MARKER in ln}
    out: list[str] = []
    i, n, line = 0, len(text), 1
    state = "code"
    changes = 0
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "\n":
            line += 1
            if state == "line_comment":
                state = "code"
            out.append(c)
            i += 1
            continue
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"; out.append(text[i:i+2]); i += 2; continue
            if c == "/" and nxt == "*":
                state = "block_comment"; out.append(text[i:i+2]); i += 2; continue
            if c == '"':
                state = "string"; out.append(c); i += 1; continue
            if c == "'":
                state = "char"; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == "line_comment":
            out.append(c); i += 1; continue
        if state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"; out.append(text[i:i+2]); i += 2; continue
            out.append(c); i += 1; continue
        # string / char
        if c == "\\":
            out.append(text[i:i+2]); i += 2; continue
        if (state == "string" and c == '"') or (state == "char" and c == "'"):
            state = "code"; out.append(c); i += 1; continue
        if ord(c) > 0x7F and c in FIX_MAP and line not in allow_lines:
            out.append(FIX_MAP[c]); changes += 1; i += 1; continue
        out.append(c); i += 1; continue
    return "".join(out), changes


def check_file(path: Path) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        return [f"{path}: cannot read ({exc})"]
    lines = text.splitlines()
    out: list[str] = []
    for line_no, what in scan(text):
        src = lines[line_no - 1] if 0 < line_no <= len(lines) else ""
        if ALLOW_MARKER in src:
            continue
        out.append(f"{path}:{line_no}: non-ASCII {what} in a string/char literal: {src.strip()}")
    return out


def iter_sources(roots: list[Path]):
    for root in roots:
        if root.is_file() and root.suffix in EXTS:
            yield root
        elif root.is_dir():
            for p in sorted(root.rglob("*")):
                if p.suffix in EXTS and p.is_file():
                    yield p


def main(argv: list[str]) -> int:
    args = argv[1:]
    do_fix = "--fix" in args
    args = [a for a in args if a != "--fix"]
    roots = [Path(a) for a in args] or [
        Path("src"), Path("tools"), Path("sdk"), Path("include")
    ]
    if do_fix:
        total = 0
        for src in iter_sources(roots):
            try:
                text = src.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            new, changes = fix_text(text)
            if changes:
                src.write_text(new, encoding="utf-8")
                total += changes
                print(f"  fixed {changes:3d} in {src}")
        print(f"check-output-ascii --fix: replaced {total} char(s). "
              "Re-run without --fix to confirm clean (unmapped non-ASCII stays).")
        # Fall through to a verification pass so the exit code reflects residue.
    findings: list[str] = []
    for src in iter_sources(roots):
        findings.extend(check_file(src))
    if findings:
        print("\n".join(findings))
        print(f"\n{len(findings)} non-ASCII literal(s) found. A UEFI text console "
              "renders these as blocks; use ASCII (e.g. '-' for an em-dash, '->' "
              f"for an arrow), or add an inline '{ALLOW_MARKER}: <why>' marker for "
              "a deliberate UTF-8 fixture.")
        return 1
    print("check-output-ascii: clean — no non-ASCII bytes in string/char literals.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
