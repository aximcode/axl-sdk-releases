#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""Every AXL command must be able to say what version it is.

Two halves, because the two sides answer by different machinery:

TARGET tools answer through AXL_TOOL_MAIN, which routes argv into
axl_version_handle() before the tool body ever runs. A tool that spells
its entry point some other way silently loses --version, and nothing
else about it looks wrong. Checked statically against TOOL_NAMES -- the
Makefile's own list, not a second copy that would drift from it.

HOST commands answer through axl_handle_version() (bash) or
axl_version.py (argparse). Checked by RUNNING them: the flag existing in
the source proves nothing, and a grep for "--version" over these scripts
returns three false positives -- a comment, a line of prose, and a
suggestion inside an error message. This gate was written after that
grep reported run-qemu.sh and install.sh as supporting a flag neither
had, which is the failure mode it exists to prevent.

The shape is "<prog> <version>", the same one AXL_TOOL_MAIN emits, so a
consumer parsing any AXL command's --version parses them all alike.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

# name on the command line -> file that implements it
HOST_COMMANDS = {
    "axl":                   SCRIPTS / "axl",
    "run-qemu":              SCRIPTS / "run-qemu.sh",
    "profile-qemu":          SCRIPTS / "profile-qemu.sh",
    "axl-prune":             SCRIPTS / "axl-prune.sh",
    "axl-install-toolchain": SCRIPTS / "install-toolchain.sh",
    "axl-emulate":           SCRIPTS / "axl-emulate",
    "extract-fv-shell":      SCRIPTS / "extract-fv-shell.py",
    "gdb-syms":              SCRIPTS / "gdb-syms.py",
    "rsod-decode":           SCRIPTS / "rsod-decode.py",
}

# axl-cc and axl-c++ REFUSE --version from a source checkout on purpose:
# the source copy resolves its SDK dir to the repo root, so it would answer
# with whatever stale prefix was last installed there. That refusal is the
# fix for a real wrong answer and must not regress into a cheerful reply.
# Staged and executable, but not a subcommand: gdb-sample.py is invoked BY
# profile-qemu.sh, never by a user, and stages 0644 so `axl` does not list it.
NOT_COMMANDS = {"gdb-sample"}

SOURCE_REFUSERS = {
    "axl-cc":  SCRIPTS / "axl-cc",
    "axl-c++": SCRIPTS / "axl-c++",
}


def fail(msg: str) -> None:
    print(f"check-tool-version: {msg}")


def main() -> int:
    errors = 0

    # ---- target tools: AXL_TOOL_MAIN, from the Makefile's own list --------
    out = subprocess.run(["make", "-s", "print-TOOL_NAMES"], cwd=ROOT,
                         capture_output=True, text=True, check=False)
    if out.returncode != 0:
        fail("could not read TOOL_NAMES from the Makefile")
        return 1
    extra = subprocess.run(["make", "-s", "print-TOOL_EXTRA_APPS"], cwd=ROOT,
                           capture_output=True, text=True, check=False)
    if extra.returncode != 0:
        fail("could not read TOOL_EXTRA_APPS from the Makefile")
        return 1
    names = out.stdout.split() + extra.stdout.split()
    if not names:
        fail("TOOL_NAMES came back EMPTY -- the gate would pass vacuously")
        return 1
    for name in names:
        src = ROOT / "tools" / f"{name}.c"
        if not src.is_file():
            fail(f"{name}: no tools/{name}.c for a name the Makefile builds")
            errors += 1
            continue
        # The INVOCATION, not the substring: "AXL_TOOL_MAIN" also occurs
        # inside AXL_TOOL_MAIN_DISABLED and inside any comment mentioning it,
        # and a gate that accepts either cannot see the thing it guards.
        #
        # EITHER mechanism counts. fbcon, crashtest and 9p have custom entry
        # points and call axl_version_handle() themselves; requiring the macro
        # would fail three tools that answer --version perfectly well.
        body = src.read_text(encoding="utf-8", errors="replace")
        if not (re.search(r"\bAXL_TOOL_MAIN\s*\(", body)
                or re.search(r"\baxl_version_handle\s*\(", body)):
            fail(f"{name}: tools/{name}.c uses neither AXL_TOOL_MAIN nor "
                 f"axl_version_handle(), so it answers no --version")
            errors += 1

    # ---- host commands: run them -----------------------------------------
    for prog, path in HOST_COMMANDS.items():
        if not path.is_file():
            fail(f"{prog}: {path.relative_to(ROOT)} is missing")
            errors += 1
            continue
        r = subprocess.run([str(path), "--version"], capture_output=True,
                           text=True, timeout=60, check=False)
        got = r.stdout.strip()
        want = f"{prog} {VERSION}"
        if r.returncode != 0 or got != want:
            fail(f"{prog}: --version said {got!r} (rc={r.returncode}), "
                 f"want {want!r}")
            errors += 1

    # ---- the two that must keep refusing ---------------------------------
    for prog, path in SOURCE_REFUSERS.items():
        r = subprocess.run([str(path), "--version"], capture_output=True,
                           text=True, timeout=60, check=False)
        blurb = (r.stdout + r.stderr).upper()
        if r.returncode == 0 or "SOURCE" not in blurb:
            fail(f"{prog}: --version from a checkout must REFUSE rather than "
                 f"answer with a stale staged version (rc={r.returncode})")
            errors += 1

    # ---- the STAGED copies, if a stage exists ----------------------------
    # The source and staged layouts are not the same shape: axl-common.sh and
    # axl_version.py live beside their callers in a checkout and under
    # libexec/axl/ once installed, and install-toolchain.sh stages to bin/
    # rather than libexec/, so its neighbour moves one directory away. That
    # exact break shipped past the source-only half of this gate.
    #
    # The version is NOT compared against the tree's: a stage legitimately
    # lags the checkout. What is checked is that each command RESOLVES one at
    # all, which is the thing the layout breaks.
    stage_bin = ROOT / "stage" / "bin"
    if not (stage_bin / "axl").is_file():
        print("check-tool-version: no stage/ -- staged-layout checks NOT run "
              "(run scripts/install.sh to cover them)")
    else:
        staged = {p_: [str(stage_bin / "axl"), c_, "--version"]
                  for p_, c_ in (("axl-emulate", "emulate"),
                                 ("extract-fv-shell", "extract-fv-shell"),
                                 ("gdb-syms", "gdb-syms"),
                                 ("profile-qemu", "profile-qemu"),
                                 ("axl-prune", "prune"),
                                 ("rsod-decode", "rsod-decode"),
                                 ("run-qemu", "run-qemu"))}
        staged["axl"] = [str(stage_bin / "axl"), "--version"]
        staged["axl-install-toolchain"] = [
            str(stage_bin / "axl-install-toolchain"), "--version"]
        for prog, argv in staged.items():
            if not Path(argv[0]).is_file():
                fail(f"staged {prog}: {argv[0]} is missing")
                errors += 1
                continue
            r = subprocess.run(argv, capture_output=True, text=True,
                               timeout=60, check=False)
            got = r.stdout.strip()
            if r.returncode != 0 or not re.fullmatch(
                    rf"{re.escape(prog)} \d+\.\d+\.\d+", got):
                fail(f"staged {prog}: --version said {got!r} "
                     f"(rc={r.returncode}), want '{prog} <x.y.z>'")
                errors += 1
        print(f"check-tool-version: staged layout checked "
              f"({len(staged)} command(s))")

    # ---- ONE owner for the host-tool file set, and both consumers read it -
    # The set ships in two layouts (install.sh -> <prefix>/libexec/axl,
    # release.yml -> the host-tools tarball). They were two hand-written lists
    # until axl_version.py went into one and not the other, which shipped a
    # tarball whose four Python tools all died on import. The Makefile owns it
    # now; this checks the owner is right AND that nobody has quietly gone back
    # to a literal list.
    ht = subprocess.run(["make", "-s", "print-HOST_TOOL_FILES"], cwd=ROOT,
                        capture_output=True, text=True, check=False)
    files = ht.stdout.split()
    if ht.returncode != 0 or not files:
        fail("could not read HOST_TOOL_FILES from the Makefile")
        errors += 1
    else:
        if "axl_version.py" not in files:
            fail("axl_version.py is not in HOST_TOOL_FILES, so every shipped "
                 "Python tool would fail to import it")
            errors += 1
        for name in files:
            if not (SCRIPTS / name).is_file():
                fail(f"HOST_TOOL_FILES names scripts/{name}, which does not exist")
                errors += 1

    # The tarball's assembly moved OUT of release.yml and into its own script
    # (D2), so that it has a local reproduction -- the workflow must therefore
    # DELEGATE rather than read the list itself, or a third copy could quietly
    # grow back inline where nothing but a release run would ever see it.
    for consumer, needle, why in (
            (SCRIPTS / "install.sh", "print-HOST_TOOL_FILES",
             "it has its own copy of the host-tool list, which is how they drifted"),
            (SCRIPTS / "make-host-tools-tarball.sh", "print-HOST_TOOL_FILES",
             "it has its own copy of the host-tool list, which is how they drifted"),
            (ROOT / ".github/workflows/release.yml", "make-host-tools-tarball.sh",
             "it assembles the host-tools tarball inline again, which puts the "
             "list back where only a release run can check it")):
        if needle not in consumer.read_text(encoding="utf-8"):
            fail(f"{consumer.name} does not reference {needle} -- {why}")
            errors += 1

    # HOST_COMMANDS is written out rather than derived, so guard it against
    # drift: every EXECUTABLE script install.sh stages must be covered here.
    # `axl` builds its command list from libexec/axl/ precisely so there is no
    # second list; this is the next best thing for a gate that must also name
    # the expected output of each one.
    if files:
        for fname in files:
            src = SCRIPTS / fname
            if not src.is_file() or not src.stat().st_mode & 0o111:
                continue          # a library, not a command
            cmd = re.sub(r"\.(sh|py)$", "", fname)
            if cmd not in HOST_COMMANDS and cmd not in NOT_COMMANDS:
                fail(f"{fname} is staged and executable but is not in "
                     f"HOST_COMMANDS -- its --version goes unchecked")
                errors += 1

    if errors:
        print(f"check-tool-version: {errors} problem(s)")
        return 1
    print(f"check-tool-version: clean -- {len(names)} target tool(s), "
          f"{len(HOST_COMMANDS)} host command(s), "
          f"{len(SOURCE_REFUSERS)} intentional refuser(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
