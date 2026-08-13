#!/usr/bin/env python3
"""check-flag-parity.py — the three build paths must agree on what matters.

An AXL image can be produced three ways, and each carries its own copy of the
compile flags and the objcopy section list:

    Makefile                 the in-tree build
    scripts/axl-cc           the SDK driver a consumer runs
    scripts/install.sh       the CMake package it generates (find_package(axl))

Copies drift, and this tree has the scar tissue to prove it. `LINT_GATES` was
duplicated between the Makefile and verify.sh until the drift started deleting
objects mid-build. Then, in one afternoon, both of these happened:

  * The stack protector was turned on in the Makefile and axl-cc and NOT in the
    CMake package, so the same source built two ways had two different security
    postures depending on which path the consumer picked.

  * The aa64 linker script's relocation output section was renamed to
    `.rela.dyn` and `-j .rela.dyn` was added to two of the three objcopy lists.
    `objcopy -j` takes EXACT names, so the third path silently dropped EVERY
    relocation -- an image that boots into mis-relocated code.

Neither is visible in a diff of the file you are editing, which is what makes
them worth a gate rather than a review habit.

What is checked is deliberately narrow: flags where disagreement is a
correctness or security defect, not style. `-Os` vs `-O2` is a legitimate
difference between paths; `-fshort-wchar` is not, because it changes the ABI.

Comment lines are stripped first. A flag named only in a comment (axl-cc's
--help text mentions `-fno-stack-protector` as the opt-out) must not satisfy
the check.

Usage: check-flag-parity.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Flags whose absence from ANY path is a defect. Each changes ABI, memory
# safety, or code generation in a way that makes two images incompatible or
# one of them unsafe.
CRITICAL_C_FLAGS = [
    "-ffreestanding",              # hosted libc headers would be wrong
    "-fshort-wchar",               # UEFI CHAR16; ABI-visible
    "-fno-builtin",                # gcc must not assume libc semantics
    "-fpic",                       # images are relocated at load
    "-fno-omit-frame-pointer",     # the RSOD unwinder walks the FP chain
    "-fstack-protector-strong",    # smash detection
    "-mstack-protector-guard=global",  # ... from a symbol, not %fs TLS
]

PATHS = {
    "Makefile": Path("Makefile"),
    "scripts/axl-cc": Path("scripts/axl-cc"),
    "scripts/install.sh": Path("scripts/install.sh"),
}


# A trailing comment starts at whitespace-then-hash. `$#` and `${VAR#pat}`
# have no preceding whitespace, so shell parameter expansion survives.
TRAILING_COMMENT_RE = re.compile(r"\s#.*$")


def uncommented(path: Path) -> str:
    """@a path with comments removed, whole-line AND trailing.

    Shell, make and cmake all use `#`. A flag mentioned only in prose must not
    count as a flag that is passed — axl-cc's --help text names
    `-fno-stack-protector` as the opt-out, and an earlier version of this gate
    that stripped only whole-line comments accepted
    `FLAGS = -fno-stack-protector  # -fstack-protector-strong` as compliant.

    Over-stripping is the safe direction: it can only cause a false FAILURE,
    which is loud. Under-stripping causes a false PASS, which is the thing
    this gate exists to prevent.
    """
    keep: list[str] = []
    for line in path.read_text().splitlines():
        if line.lstrip().startswith("#"):
            continue
        keep.append(TRAILING_COMMENT_RE.sub("", line))
    return "\n".join(keep)


def objcopy_sections(text: str) -> set[str]:
    """The `-j <section>` set of the first objcopy invocation found."""
    return set(re.findall(r"-j\s+(\.[A-Za-z0-9_.]+)", text))


def main(argv: list[str]) -> int:
    missing_files = [n for n, p in PATHS.items() if not p.exists()]
    if missing_files:
        print(f"check-flag-parity: FAIL — missing {', '.join(missing_files)}")
        return 1

    bodies = {name: uncommented(path) for name, path in PATHS.items()}
    problems: list[str] = []

    for flag in CRITICAL_C_FLAGS:
        absent = [name for name, body in bodies.items() if flag not in body]
        if absent:
            present = [n for n in bodies if n not in absent]
            problems.append(
                f"{flag}: present in {', '.join(present) or 'nothing'} but "
                f"MISSING from {', '.join(absent)}")

    # objcopy -j sets. A section carried by one path and not another is a
    # silently different image, and .rela.dyn showed that can mean "no
    # relocations at all".
    sections = {name: objcopy_sections(body) for name, body in bodies.items()}
    union: set[str] = set()
    for s in sections.values():
        union |= s
    for name, have in sections.items():
        gap = union - have
        if gap:
            problems.append(
                f"objcopy -j: {name} does not carry {', '.join(sorted(gap))} "
                f"(other paths do). `-j` takes EXACT section names.")

    if problems:
        print("check-flag-parity: FAIL — the build paths disagree on flags that "
              "change ABI, safety or relocation:")
        for p in problems:
            print(f"  {p}")
        print("  Every path must produce an equivalent image. Fix the one that "
              "drifted, do not relax this list.")
        return 1

    print(f"check-flag-parity: clean — {len(CRITICAL_C_FLAGS)} critical flags and "
          f"{len(union)} objcopy sections agree across {len(PATHS)} build paths")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
