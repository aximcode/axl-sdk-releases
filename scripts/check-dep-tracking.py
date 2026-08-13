#!/usr/bin/env python3
"""check-dep-tracking.py — every C/C++ object must be built with `-MD -MP`.

Without them the object has NO header dependency, so editing a header rebuilds
nothing and the next run tests the PREVIOUS binary. That is a wrong-answer
generator rather than a slow-build annoyance: `CXXFLAGS` was missing them for
the entire life of the C++ layer, and it made a sabotage of `axl-istream.hpp`
report as UNDETECTED because the code was never recompiled.

`$(BUILDDIR)/.axl-build-state` does not cover this. It hashes WHICH FLAGS an
object was built with, not WHICH HEADERS it depends on — a header edit changes
neither the flags nor the .c file, so nothing in the signature moves.

What this checks: for every Makefile recipe that compiles a declared `.c` /
`.cpp` prerequisite, the flag variable it uses must expand to something
containing `-MD`. Assembly is exempt on evidence, not on faith — a `.S` still
runs through cpp, so it is exempt only while it includes nothing.

What it CANNOT see: compiles whose sources are discovered at runtime rather
than declared as prerequisites (`check-examples` globs a directory). Those are
listed in EXEMPT_RULES with a reason, so the blind spot is visible rather than
accidental.
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAKEFILE = REPO / "Makefile"

# Rules whose compiles this gate structurally cannot inspect, each with the
# reason it is safe. Named individually -- a blanket "skip check-* targets"
# would silently absorb a future gate that DOES build a real object.
EXEMPT_RULES: dict[str, str] = {
    "check-examples": (
        "compiles sdk/examples/* discovered by a shell glob, into throwaway "
        "objects; nothing links them, so a stale one cannot mislead"
    ),
    "check-cxx-entry": (
        "compiles one throwaway probe object to inspect its entry-point "
        "mangling; discarded in the same recipe"
    ),
}

# Source suffixes that can carry a #include and therefore need dependency
# tracking. `.S` is deliberately absent -- see verify_assembly_has_no_includes.
NEEDS_DEPS = (".c", ".cpp", ".cc", ".cxx")
ASSEMBLY = (".S", ".s")


def expand(vars_: list[str]) -> dict[str, str]:
    """Ask make itself for the expanded value of each variable.

    Expanding by hand would re-implement make's flavours and get
    `$(filter-out ...)` wrong -- CXXFLAGS_HOSTED is DERIVED from CXXFLAGS, so
    a textual grep reports it as having no -MD when it does. That mistake was
    made once already while auditing this very problem.
    """
    # OUTSIDE the repo: a probe written into the tracked tree survives a
    # signal, and two concurrent gate runs would race on the one path. cwd is
    # what makes `include Makefile` resolve, not the probe's location.
    #
    # The goal name must start with `print-`, which the Makefile's
    # NONCLEAN_GOALS filters out -- otherwise this grandchild make runs the
    # build-state block and can wipe a live $(BUILDDIR) while verify.sh is
    # building into it.
    with tempfile.TemporaryDirectory(prefix="axl-dep-probe.") as td:
        probe = Path(td) / "probe.mk"
        probe.write_text('include Makefile\nprint-%: ; @echo "$($*)"\n')
        out: dict[str, str] = {}
        for v in vars_:
            r = subprocess.run(
                ["make", "-s", "-f", str(probe), f"print-{v}"],
                cwd=REPO, capture_output=True, text=True,
            )
            if r.returncode != 0:
                # Distinguished from "expanded to something without -MD":
                # reporting a broken probe as a missing flag sends the reader
                # to the wrong file entirely.
                print(f"check-dep-tracking: FAIL - could not expand $({v}); "
                      f"the probe is broken, not the build.\n{r.stderr.strip()}")
                sys.exit(1)
            out[v] = r.stdout.strip()
        return out


def parse_compiles() -> list[tuple[int, str, str, str]]:
    """(line_no, rule_target, flag_var, first_prereq) per compile recipe."""
    rule_re = re.compile(r"^([^\t#=\s][^=]*?):(?!=)(.*)$")
    cc_re = re.compile(r"\$\((?:CC|CXX)\)\s+\$\(([A-Z_0-9]+)\)")
    found: list[tuple[int, str, str, str]] = []
    target, prereqs = "", ""

    for n, line in enumerate(MAKEFILE.read_text().splitlines(), 1):
        if not line.startswith("\t"):
            m = rule_re.match(line)
            if m:
                target, prereqs = m.group(1).strip(), m.group(2)
            continue
        if " -c " not in line and not line.rstrip().endswith(" -c"):
            continue
        m = cc_re.search(line)
        if m:
            first = prereqs.split("|")[0].split()
            found.append((n, target, m.group(1), first[0] if first else ""))
    return found


def suffix_of(prereq: str, expanded: dict[str, str]) -> str:
    """The source suffix a prerequisite refers to, resolving one $(VAR)."""
    m = re.fullmatch(r"\$\(([A-Za-z_0-9]+)\)", prereq)
    if m:
        prereq = expanded.get(m.group(1), "").split()[0] if expanded.get(m.group(1)) else ""
    return Path(prereq).suffix if prereq else ""


def assembly_without_includes() -> list[str]:
    """Assembly sources that DO include a header, i.e. wrongly exempt."""
    bad = []
    for path in list(REPO.glob("src/**/*.S")) + list(REPO.glob("experiments/**/*.S")):
        text = path.read_text(errors="replace")
        if re.search(r"^\s*#\s*include", text, re.M):
            bad.append(str(path.relative_to(REPO)))
    return bad


def main() -> int:
    compiles = parse_compiles()

    # Positive control. A parse that matched nothing would report "clean" --
    # the failure mode this whole file exists to prevent.
    if len(compiles) < 5:
        print(f"check-dep-tracking: FAIL — parsed only {len(compiles)} compile "
              f"recipes from the Makefile; the parser is broken, not the build.")
        return 1

    vars_needed = sorted({v for _, _, v, _ in compiles})
    prereq_vars = sorted({
        m.group(1) for _, _, _, p in compiles
        if (m := re.fullmatch(r"\$\(([A-Za-z_0-9]+)\)", p))
    })
    expanded = expand(vars_needed + prereq_vars)

    failures: list[str] = []
    checked = 0

    for line_no, target, flag_var, prereq in compiles:
        rule = target.split()[0] if target else ""
        if rule in EXEMPT_RULES:
            continue

        suffix = suffix_of(prereq, expanded)
        if suffix in ASSEMBLY or not suffix:
            continue
        if suffix not in NEEDS_DEPS:
            continue

        checked += 1
        if "-MD" not in expanded.get(flag_var, ""):
            failures.append(
                f"  Makefile:{line_no}: {target}\n"
                f"      compiles {prereq} with $({flag_var}), which has no -MD -MP.\n"
                f"      That object will not rebuild when a header it includes changes."
            )

    stray = assembly_without_includes()
    if stray:
        failures.append(
            "  assembly sources are exempt only because they include nothing,\n"
            "  and these now do — give them a flag set carrying -MD -MP:\n"
            + "".join(f"      {s}\n" for s in stray)
        )

    if failures:
        print("check-dep-tracking: FAIL\n")
        print("\n".join(failures))
        print("\n  Use $(CFLAGS) / $(CXXFLAGS) / $(CXXFLAGS_HOSTED) for real objects.")
        print("  The *_BASE variants deliberately omit -MD -MP: they are for")
        print("  throwaway probe compiles that must not scatter .d files.")
        return 1

    print(f"check-dep-tracking: clean — {checked} C/C++ compile rule(s) carry "
          f"-MD -MP, {len(EXEMPT_RULES)} rule(s) exempt by name.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
