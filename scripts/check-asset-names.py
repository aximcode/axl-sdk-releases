#!/usr/bin/env python3
"""check-asset-names.py -- one format string, four places that spell it.

AXL-Distribution-Design.md §14.1a settles a single expression for every release
asset::

    axl-sdk-${component}-${ver}${arch:+-$arch}.tar.gz

and names the reason it needs a gate: "`install.sh` constructs these names,
`release.yml` emits them and `SHA256SUMS` lists them, so a single expression in
three places beats a special case in each."  Three places that cannot share
code.  ``packaging/install.sh`` is the one that makes sharing impossible -- it
runs on a machine with nothing of ours on it, so it cannot source a helper, and
a rename on either side is invisible until a user's ``axl update`` cannot find
its download.

WHAT THIS COMPARES.  Computed names, not spellings.  The producer,
``scripts/make-release-assets.sh`` (CPack since 2026-11-10), is ASKED
(``<kind> --print-name``; uefi-tools once per arch);
``install.sh``'s ``asset_candidates()`` is extracted and RUN.  Only
``release.yml`` is parsed, because a workflow cannot be executed here -- and
what is parsed there is the one place a name is still written down, the
release job's ``cp --`` list.

So a variable renamed, an indirection added, or a component spelled differently
cannot pass: the gate never reads the format string, it reads the result.

WHAT IT DOES NOT COVER.  ``install.sh``'s LEGACY candidates -- the names
published before the rename.  Those are historical facts about releases already
on GitHub, not a scheme anything still emits, so they are deliberately exempt
from parity.  What IS checked about them is that they come second: the first
candidate per component is the current one, and the ordering is what makes
``axl use <older>`` prefer today's name and fall back.
"""

from __future__ import annotations

import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INSTALLER = ROOT / "packaging" / "install.sh"
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
MAKER = ROOT / "scripts" / "make-release-assets.sh"

def _version() -> str:
    """The version whose names are checked: VERSION, or `--version V` -- the
    seam that lets a test hold the gate to a prerelease's asset set (no
    .deb/.rpm) and a release's (both) from one tree."""
    if len(sys.argv) == 3 and sys.argv[1] == "--version":
        return sys.argv[2]
    if len(sys.argv) != 1:
        print("usage: check-asset-names.py [--version V]", file=sys.stderr)
        sys.exit(2)
    return (ROOT / "VERSION").read_text().strip()


VERSION = _version()
UEFI_ARCHES = ("x64", "aa64")

# The settled scheme, with the version as a LITERAL rather than a pattern.
# `component` may itself contain dashes (host-tools, uefi-tools) and so may a
# prerelease version, so a `\d+\.\d+\.\d+(-...)?` version group swallows the
# arch field instead: it read `axl-sdk-uefi-tools-4.4.0-aa64` as version
# "4.4.0-aa64" and reported the arch as a bad version. Pinning the version we
# already know removes the ambiguity and checks the version at the same time.
SETTLED_SHAPE = "axl-sdk-<component>-%s[-<arch>].tar.gz" % VERSION
SETTLED = re.compile(
    r"^axl-sdk-(?P<component>[a-z0-9]+(?:-[a-z0-9]+)*)"
    r"-" + re.escape(VERSION) +
    r"(?:-(?P<arch>[a-z0-9_]+))?\.tar\.gz$"
)


def fail(msg: str) -> None:
    print(f"check-asset-names: {msg}", file=sys.stderr)


def normalize_host_arch(name: str) -> str:
    """Fold the machine's own arch and the literal x86_64 to one token.

    make-release-assets.sh names the SDK archive after `uname -m`, while release.yml
    and install.sh spell `x86_64` because that is what the release builds on.
    Comparing them literally would report a false drift on an aarch64 host and
    say nothing useful; folding both keeps the gate meaningful everywhere.
    """
    return name.replace(f"-{platform.machine()}.tar.gz", "-<hostarch>.tar.gz") \
               .replace("-x86_64.tar.gz", "-<hostarch>.tar.gz")


def run(cmd: list[str]) -> str | None:
    """Run a command, distinguishing "could not run" from "ran and said nothing".

    Empty output and a failed exec are the same empty string and opposite
    facts; the caller needs to be able to tell them apart, so a non-zero exit
    or a missing binary is None, never "".
    """
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, timeout=60)
    except (OSError, subprocess.SubprocessError) as exc:
        fail(f"could not run {' '.join(cmd)}: {exc}")
        return None
    if proc.returncode != 0:
        fail(f"{' '.join(cmd)} exited {proc.returncode}: {proc.stderr.strip()}")
        return None
    return proc.stdout.strip()


def installer_candidates() -> dict[str, list[str]] | None:
    """Extract asset_candidates() from install.sh and run it per component."""
    src = INSTALLER.read_text()
    match = re.search(r"^asset_candidates\(\) \{\n(.*?)^\}\n", src, re.M | re.S)
    if match is None:
        fail(f"no asset_candidates() function in {INSTALLER.relative_to(ROOT)} --"
             " it changed shape and this gate must follow it")
        return None
    body = match.group(0)
    out: dict[str, list[str]] = {}
    for component in ("sdk", "host-tools"):
        script = f'COMPONENT={component}\nVERSION={VERSION}\n{body}\nasset_candidates\n'
        got = run(["sh", "-c", script])
        if got is None:
            return None
        names = [line.strip() for line in got.splitlines() if line.strip()]
        if not names:
            fail(f"asset_candidates() named nothing for COMPONENT={component}")
            return None
        out[component] = names
    return out


def uefi_names() -> set[str] | None:
    """The uefi-tools archive names, asked of the script once per arch."""
    names: set[str] = set()
    for arch in UEFI_ARCHES:
        got = run([str(MAKER), "uefi-tools", "--arch", arch, "--version", VERSION,
                   "--print-name"])
        if got is None:
            return None
        if not got:
            fail(f"{MAKER.name} uefi-tools --print-name named nothing for {arch}")
            return None
        names.add(got)
    return names


def workflow_names() -> set[str] | None:
    """The names release.yml's release job PUBLISHES."""
    src = WORKFLOW.read_text()

    published = {
        m.replace("${V}", VERSION)
        for m in re.findall(r'^\s*cp -- "dist/(axl-sdk-[^"]*\.(?:tar\.gz|deb|rpm))"', src, re.M)
    }
    if not published:
        fail("the release job copies no dist/axl-sdk-*.tar.gz -- it changed shape"
             " and this gate must follow it")
        return None
    return published


def main() -> int:
    if shutil.which("sh") is None:
        fail("no sh on PATH")
        return 1

    sdk = run([str(MAKER), "sdk", "--version", VERSION, "--print-name"])
    host = run([str(MAKER), "host-tools", "--version", VERSION, "--print-name"])
    # THE PUBLISHED SET, from the one place it is decided: the driver's
    # `assets` (the archives, install.sh, VERSION, and the .deb/.rpm unless
    # VERSION is a prerelease). release.yml asks the same question.
    assets_out = run([str(MAKER), "assets", "--version", VERSION])
    candidates = installer_candidates()
    emitted_uefi = uefi_names()
    published = workflow_names()
    if (sdk is None or host is None or assets_out is None or candidates is None
            or emitted_uefi is None or published is None):
        return 1
    assets = {line for line in assets_out.splitlines() if line}
    packages = {n for n in assets if n.endswith((".deb", ".rpm"))}

    errors = 0

    # 1. Every name any producer emits matches the settled format.
    produced = {sdk, host} | emitted_uefi
    for name in sorted(produced):
        if SETTLED.match(name) is None:
            fail(f"{name!r} does not match {SETTLED_SHAPE}")
            errors += 1

    # 2. install.sh's CURRENT name for each component is what that producer
    #    emits. This is the pairing that no shared code can enforce.
    for component, producer in (("sdk", sdk), ("host-tools", host)):
        current = candidates[component][0]
        if normalize_host_arch(current) != normalize_host_arch(producer):
            fail(f"{component}: install.sh fetches {current!r} but the producer"
                 f" emits {producer!r}")
            errors += 1

    # 3. install.sh's fallbacks come AFTER the current name and are not
    #    themselves current-format duplicates of it.
    for component, names in candidates.items():
        if len(names) != len(set(names)):
            fail(f"{component}: duplicate entries in asset_candidates(): {names}")
            errors += 1
        if len(names) < 2:
            fail(f"{component}: no legacy fallback -- `axl use <older>` cannot"
                 " reach releases published before the rename")
            errors += 1

    # 4. The driver's asset set holds exactly what the producers emit, plus
    #    the two stable names and the packages it decides on.
    if {normalize_host_arch(n) for n in assets - packages - {"install.sh", "VERSION"}} \
            != {normalize_host_arch(n) for n in produced}:
        fail("the driver's `assets` disagrees with the archives its kinds name:")
        fail(f"  assets:    {sorted(assets)}")
        fail(f"  produced:  {sorted(produced)}")
        errors += 1
    for name in sorted(packages):
        kind = "deb" if name.endswith(".deb") else "rpm"
        got = run([str(MAKER), kind, "--version", VERSION, "--print-name"])
        if got != name:
            fail(f"`assets` lists {name!r} but `{kind} --print-name` says {got!r}")
            errors += 1

    # 5. The release job publishes exactly that set. Its package cp lines sit
    #    under the driver's own `assets` test, so for a version with no
    #    packages (a prerelease) they are not part of what it stages.
    if not packages:
        published = {n for n in published if not n.endswith((".deb", ".rpm"))}
    produced_all = produced | packages
    if {normalize_host_arch(n) for n in published} != {normalize_host_arch(n) for n in produced_all}:
        fail("the release job's asset list disagrees with what the build jobs emit:")
        fail(f"  produced:  {sorted(produced_all)}")
        fail(f"  published: {sorted(published)}")
        errors += 1

    if errors:
        print(f"check-asset-names: FAIL -- {errors} problem(s)", file=sys.stderr)
        return 1

    print(f"check-asset-names: clean -- {len(produced)} archives and {len(packages)} packages for {VERSION}, "
          f"{sum(len(v) for v in candidates.values())} installer candidates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
