#!/usr/bin/env python3
"""check-toolchain-conf -- keep the C++ cross-toolchain paths in ONE place.

Two failures, both of which have precedent in this tree:

1. INTERNAL CONSISTENCY. scripts/axl-toolchains.conf spells every path in full
   because its KEY=VALUE subset has to parse as both `make` and `sh`, and
   neither reads the other's interpolation syntax. That repetition is the price
   of the shared format, so it gets asserted rather than trusted: the version
   must appear in the directory, and the directory must prefix the compiler.

2. DRIFT. The AArch64 path used to be written out in five places -- the
   Makefile, scripts/axl-cc, two spots in scripts/install.sh (one inside the
   GENERATED axl-config.cmake), and the installer. Adding x64 would have made
   ten. `make check-flag-parity` exists because the same three build paths
   drifted on compile flags twice in one afternoon; the toolchain path had no
   equivalent guard. So: no build-critical file may hardcode a toolchain path.
   They must read the manifest.

Docs are deliberately NOT scanned -- prose quoting a path is describing it, not
depending on it, and rewriting that prose in a sweep is its own bug class.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# The x64 builder carries its OWN default version (GCC_VER) and derives its
# install prefix from it. That is the file where a version bump is actually
# made, so leaving it unchecked would let the manifest and the builder disagree
# -- exactly the "one was bumped without the other" failure this gate names.
BUILDER = "toolchain/x86_64-elf/build-toolchain.sh"
# `14.3.0` plus AXL's optional `-axlN` build revision, stopping before
# `.tar.xz` so a tarball name yields the version and not the suffix. It does
# NOT read ARM's `14.3.rel1` shape -- x64 does not use it, and if x64 ever
# adopted one the gate would hard-fail with "no version string found" rather
# than pass silently, which is the right direction to be wrong in.
VERSION_IN_URL_RE = re.compile(r"\d+\.\d+\.\d+(?:-[A-Za-z]+\d+)?")
BUILDER_VER_RE = re.compile(r'^GCC_VER="\$\{GCC_VER:-([^}"]+)\}"', re.MULTILINE)
# AXL's own build revision on top of the upstream GCC version. The manifest
# version is GCC_VER + AXL_REV ("14.3.0" + "-axl"), because a rebuild with
# different configure flags is a different toolchain while being the same
# upstream release. Comparing GCC_VER alone reported a false mismatch.
BUILDER_REV_RE = re.compile(r'^AXL_REV="\$\{AXL_REV:-([^}"]*)\}"', re.MULTILINE)

REPO = Path(__file__).resolve().parent.parent
CONF = REPO / "scripts" / "axl-toolchains.conf"

# Files that must resolve paths through the manifest rather than restate them.
SCANNED: list[str] = [
    "Makefile",
    "scripts/axl-cc",
    "scripts/axl-c++",
    "scripts/install.sh",
    "scripts/install-toolchain.sh",
    "scripts/install-arm-toolchain.sh",
    # The workflow that CALLS the installer: an `export PATH=/opt/arm-gnu-...`
    # added here would pin a version the manifest no longer names.
    ".github/workflows/release.yml",
    ".github/workflows/ci.yml",
    "sdk/examples/CMakeLists.txt",
]

# A hardcoded path looks like one of these roots.
PATH_ROOTS = (
    "/opt/arm-gnu-toolchain",
    "/opt/x86_64-elf-gcc",
)


def parse_conf(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            sys.exit(f"check-toolchain-conf: FAIL -- not KEY=VALUE: {raw!r}")
        key, _, value = line.partition("=")
        # The dual sh/make contract: no spaces around '=', no interpolation.
        if key != key.strip() or value != value.strip():
            sys.exit(
                f"check-toolchain-conf: FAIL -- spaces around '=' break `make`"
                f" parsing: {raw!r}"
            )
        if "$" in value:
            sys.exit(
                "check-toolchain-conf: FAIL -- interpolation is not portable"
                f" between sh and make: {raw!r}"
            )
        out[key] = value
    return out


def main() -> int:
    if not CONF.is_file():
        print(f"check-toolchain-conf: FAIL -- missing {CONF}")
        return 1

    conf = parse_conf(CONF.read_text())
    errors: list[str] = []

    for arch in ("AA64", "X64"):
        ver = conf.get(f"AXL_{arch}_TOOLCHAIN_VERSION")
        dir_ = conf.get(f"AXL_{arch}_TOOLCHAIN_DIR")
        gxx = conf.get(f"AXL_{arch}_GXX_DEFAULT")
        if not (ver and dir_ and gxx):
            errors.append(f"{arch}: missing one of VERSION / DIR / GXX_DEFAULT")
            continue
        if ver not in dir_:
            errors.append(
                f"{arch}: version {ver!r} does not appear in directory {dir_!r}"
                " -- one was bumped without the other"
            )
        if not gxx.startswith(dir_ + "/"):
            errors.append(
                f"{arch}: compiler {gxx!r} is not inside directory {dir_!r}"
            )

    # The aa64 tarball is fetched over the network, so its checksum is the only
    # thing standing between a bumped version and an unverified binary.
    sha = conf.get("AXL_AA64_TOOLCHAIN_SHA256", "")
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        errors.append(
            "AA64: AXL_AA64_TOOLCHAIN_SHA256 must be 64 lowercase hex chars,"
            f" got {sha!r} -- the installer refuses to run without it"
        )

    # X64's URL is a LITERAL; aa64's is built from its VERSION
    # (install-toolchain.sh: AA64_URL=".../${AXL_AA64_TOOLCHAIN_VERSION}/..."),
    # so only this one can name a different version than the manifest declares.
    #
    # WHAT THAT COSTS when it drifts, which is why it is a gate and not a
    # comment: the download SUCCEEDS, the sha of the OLD tarball MATCHES, it
    # extracts to the old version's directory -- and $gxx, built from DIR,
    # is still missing. install_x64 then falls through to a ~40 MINUTE SOURCE
    # BUILD of GCC. Everything looks like it is working. Found by a prune test
    # that created the inconsistency by hand.
    # `x_ver in x_url` is NOT enough, and the first draft of this check proved
    # it: the URL carries the version TWICE -- once as the release tag, once in
    # the tarball name -- so changing only the filename left the tag matching
    # and the substring test satisfied. Every version-looking string in the two
    # significant components must BE the declared version; a mismatch in either
    # half is a different failure and both end in a 40-minute build.
    # REQUIRED, not "checked if present". The aa64 sibling above fails on an
    # empty string and these did not, so deleting either key passed the gate --
    # and install_x64 gates the whole tarball path on `[[ -n "$url" && -n
    # "$sha" ]]`, so a missing key falls straight through to the 40-minute
    # source build this check exists to prevent. Absence IS the failure.
    x_ver = conf.get("AXL_X64_TOOLCHAIN_VERSION", "")
    x_url = conf.get("AXL_X64_TOOLCHAIN_URL", "")
    if not x_url:
        errors.append(
            "X64: AXL_X64_TOOLCHAIN_URL is missing -- install_x64 skips the"
            " prebuilt tarball entirely without it and source-builds instead"
        )
    elif x_ver:
        parts = x_url.rstrip("/").split("/")[-2:]      # <tag>/<tarball>
        seen = [v for part in parts for v in VERSION_IN_URL_RE.findall(part)]
        if not seen:
            errors.append(
                f"X64: no version string found in AXL_X64_TOOLCHAIN_URL"
                f" {x_url!r} -- this check cannot tell whether it agrees with"
                f" the declared {x_ver!r}, which is not the same as agreeing"
            )
        elif any(v != x_ver for v in seen):
            errors.append(
                f"X64: AXL_X64_TOOLCHAIN_URL names {sorted(set(seen))} but the"
                f" manifest declares {x_ver!r} -- the download would deliver a"
                " different version than DIR names, the sha of THAT tarball"
                " would match, and the installer would source-build for ~40"
                " minutes without ever saying why"
            )

    # Same rule as aa64's, one sentinel wider: install_x64 treats
    # PENDING_UPLOAD as "no prebuilt yet" and source-builds deliberately, which
    # is a decision rather than the accident above.
    x_sha = conf.get("AXL_X64_TOOLCHAIN_SHA256", "")
    if x_sha != "PENDING_UPLOAD" and not re.fullmatch(r"[0-9a-f]{64}", x_sha):
        errors.append(
            "X64: AXL_X64_TOOLCHAIN_SHA256 must be 64 lowercase hex chars or"
            f" PENDING_UPLOAD, got {x_sha!r} -- an absent one makes install_x64"
            " skip the tarball and source-build"
        )

    # Manifest vs the builder that actually produces the x64 toolchain.
    builder = REPO / BUILDER
    if builder.is_file():
        m = BUILDER_VER_RE.search(builder.read_text())
        if m is None:
            errors.append(
                f"{BUILDER}: could not find the GCC_VER default; this gate"
                " can no longer compare it against the manifest"
            )
        elif (m.group(1) + (BUILDER_REV_RE.search(builder.read_text()).group(1)
                            if BUILDER_REV_RE.search(builder.read_text()) else "")
              ) != conf.get("AXL_X64_TOOLCHAIN_VERSION"):
            errors.append(
                f"{BUILDER}: builds GCC {m.group(1)} + AXL_REV, but the manifest declares"
                f" {conf.get('AXL_X64_TOOLCHAIN_VERSION')} -- the installed"
                " toolchain would not be at the path axl-cc looks in"
            )

    for rel in SCANNED:
        path = REPO / rel
        if not path.is_file():
            continue
        for num, line in enumerate(path.read_text().splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue  # a comment naming the path is documentation
            for root in PATH_ROOTS:
                if root in line:
                    errors.append(
                        f"{rel}:{num}: hardcodes {root}... -- read it from"
                        " scripts/axl-toolchains.conf instead"
                    )

    if errors:
        print("check-toolchain-conf: FAIL")
        for e in errors:
            print(f"    {e}")
        return 1

    n = len([k for k in conf if k.endswith("_GXX_DEFAULT")])
    print(
        f"check-toolchain-conf: clean -- {n} toolchains, "
        f"{len(SCANNED)} consumers read the manifest."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
