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
BUILDER_VER_RE = re.compile(r'^GCC_VER="\$\{GCC_VER:-([^}"]+)\}"', re.MULTILINE)

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

    # Manifest vs the builder that actually produces the x64 toolchain.
    builder = REPO / BUILDER
    if builder.is_file():
        m = BUILDER_VER_RE.search(builder.read_text())
        if m is None:
            errors.append(
                f"{BUILDER}: could not find the GCC_VER default; this gate"
                " can no longer compare it against the manifest"
            )
        elif m.group(1) != conf.get("AXL_X64_TOOLCHAIN_VERSION"):
            errors.append(
                f"{BUILDER}: builds GCC {m.group(1)}, but the manifest declares"
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
