#!/bin/bash
# make-sdk-tarball.sh — build the relocatable SDK tarball.
#
# WHY THIS EXISTS. The SDK reached consumers only as a .deb/.rpm. Arch, Alpine,
# NixOS, SUSE, CI containers and locked-down corporate hosts had no supported
# install path at all, and a version pin could only be held by keeping a whole
# source checkout -- which is why ~/axl-sdk-2.2.0 and friends on this machine
# are source trees rather than prefixes. AXL-SDK-Design.md documented this
# workflow in the present tense for a long time and it had never been built;
# AXL-Distribution-Design.md §5.1 called it "the gap".
#
# It is deliberately thin. `install.sh --prefix` already produces a complete,
# relocatable prefix -- axl.pc uses ${pcfiledir}, axl-config.cmake computes
# paths relative to itself, and axl-cc/axl resolve their prefix from $0 -- so
# the only thing missing was an archive of it, plus the documentation and
# licence payload the retired .deb/.rpm used to carry (staged below). Building
# it here rather than inline in release.yml means the release and
# test/integration/test-sdk-tarball.sh run the SAME code. That argument used to
# be made against the host-tools tarball, which WAS assembled inline and had no
# local reproduction; it now has scripts/make-host-tools-tarball.sh for exactly
# this reason.
#
# ONE ARCHIVE, BOTH TARGET ARCHES. The name carries the HOST platform, because
# that is what constrains who can run it: the bundled cross toolchains are
# x86_64-hosted and bin/pe-set-debug is an ELF x86-64 binary. §5.3 measured a
# per-target split as saving ~20 MB for the cost of three packages and three
# smoke tests, and leaned against it on its own merits.
#
# THE NAME FOLLOWS ONE FORMAT STRING, `axl-sdk-<component>-<ver>[-<arch>]`
# (§14.1a), so the component is `linux` and the arch is the host machine:
# axl-sdk-linux-<ver>-x86_64.tar.gz. That splits `linux-x86_64`, which IS a
# recognisable platform token elsewhere -- recorded rather than hidden,
# because uniformity across the three assets is what release.yml, install.sh
# and SHA256SUMS all key on, and the uefi-tools archive has no OS component to
# keep a platform token consistent with.
#
# THE TOP-LEVEL DIRECTORY IS THE VERSIONED ROOT. `axl-sdk-<version>/`, so
# `tar xf ... -C /opt` yields /opt/axl-sdk-<version> with nothing to rename --
# the layout AXL-Distribution-Design.md §12.2 wants.
#
# Usage: scripts/make-sdk-tarball.sh [--out DIR] [--arch all|x64|aa64]
set -euo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

OUT_DIR="$SDK_DIR/dist"
ARCH="all"
PRINT_NAME=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)    OUT_DIR="${2:?--out needs a directory}"; shift 2 ;;
        --out=*)  OUT_DIR="${1#--out=}"; shift ;;
        --arch)   ARCH="${2:?--arch needs a value}"; shift 2 ;;
        --arch=*) ARCH="${1#--arch=}"; shift ;;
        # Report the archive's name and build nothing. scripts/check-asset-
        # names.py asks the producers what they will emit rather than reading
        # the spelling out of them, so the gate compares COMPUTED names --
        # a variable renamed or an indirection added cannot fool it.
        --print-name) PRINT_NAME=1; shift ;;
        -h|--help)
            sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "ERROR: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

VERSION="$(cat "$SDK_DIR/VERSION")"
# NAME is the versioned root INSIDE the archive (§12.2) and is deliberately
# NOT the archive's stem: the root says which SDK this is, the filename says
# which host it runs on.
NAME="axl-sdk-${VERSION}"
TARBALL="axl-sdk-linux-${VERSION}-$(uname -m).tar.gz"

if [[ "$PRINT_NAME" == "1" ]]; then
    echo "$TARBALL"
    exit 0
fi

# Stage into a temp dir rather than into $OUT_DIR: the staging path must not
# survive into the archive in any form, and the cleanest way to be sure of
# that is for it never to be a path the consumer could inherit.
STAGE_ROOT="$(mktemp -d -t axl-sdktar.XXXXXXXX)"
trap 'rm -rf "$STAGE_ROOT"' EXIT
PREFIX="$STAGE_ROOT/$NAME"

echo "[make-sdk-tarball] staging $NAME (arch: $ARCH) ..."
# --cpp so the archive can build C++ too; without it a consumer gets an SDK
# whose axl-c++ reports a missing runtime, which is a worse failure than a
# larger download.
"$SDK_DIR/scripts/install.sh" --arch "$ARCH" --cpp --prefix "$PREFIX" >/dev/null

# ---------------------------------------------------------------------------
# The documentation and licence payload.
#
# THIS IS WHAT THE PACKAGES CARRIED. The .deb/.rpm staged 75 doc files and this
# tarball staged none, which was survivable only while the packages existed. D2
# retires them (§17), so the archive that ships libaxl.a is now the archive that
# owes the obligations of everything compiled INTO libaxl.a -- and four of the
# five are obligations, not courtesies:
#
#   mbedtls   Apache-2.0 §4(a): carry the licence with any binary redistribution
#   dejavu    Bitstream Vera: the notice must accompany binary redistribution
#   libvterm  MIT, with no public-domain election -- unlike stb / sdefl / LZMA
#   freetype  FTL carries a CREDIT clause for products shipping the path-
#             filling APIs; FTL.TXT is the licence, LICENSE.TXT the dual-
#             licence statement naming it and GPLv2
#   edk2      BSD-2-Clause-Patent, for the RamDiskDxe.efi embedded in mkrd.efi
#
# NOTICE is Apache-2.0 4(d) and was itself once omitted from the package: it
# states the FreeType credit clause and the libvterm MIT notice, so leaving it
# out left both obligations unstated.
DOC="$PREFIX/share/doc/axl-sdk"
mkdir -p "$DOC"
install -m 0644 "$SDK_DIR/CHANGELOG.md" "$SDK_DIR/README.md" "$SDK_DIR/LICENSE" \
                "$SDK_DIR/NOTICE" "$SDK_DIR/THIRD_PARTY.md" "$DOC/"

# The examples ship as SOURCE. A dirty checkout leaves .efi and .o beside them,
# and build output from the maintainer's machine has no business in a release,
# least of all in the directory that carries the licences.
cp -r "$SDK_DIR/sdk/examples" "$DOC/"
# `*.txt` is in the keep-list for embed-asset.txt, which embed-asset.c
# documents as its `--embed` input -- the example is unbuildable without it.
# The filter was inherited verbatim from the retired .deb staging, where the
# omission was survivable because the examples also reached users another way;
# this archive is now the only channel that ships them.
find "$DOC/examples" -type f \
    ! -name '*.c' ! -name '*.h' \
    ! -name '*.cpp' ! -name '*.hpp' \
    ! -name '*.md' ! -name '*.txt' ! -name 'CMakeLists.txt' \
    -delete

# One table, so adding a vendored dependency is one line rather than four.
# Paths are relative to the source tree; the leaf directory names the project.
while read -r _dest _src; do
    [ -n "$_dest" ] || continue
    mkdir -p "$DOC/third_party/$_dest"
    # shellcheck disable=SC2086  # $_src is a deliberate multi-file list
    install -m 0644 $(printf "$SDK_DIR/%s " $_src) "$DOC/third_party/$_dest/"
done <<'THIRD_PARTY'
mbedtls   deps/mbedtls/LICENSE
dejavu    third_party/dejavu/LICENSE
libvterm  deps/libvterm/LICENSE
freetype  deps/freetype/FTL.TXT deps/freetype/LICENSE.TXT
edk2      third_party/edk2/LICENSE third_party/edk2/README.md
THIRD_PARTY

# A VERSION file at the root, matching the host-tools tarball. share/axl/version
# is what the tools read; this one is for a human holding an extracted tree.
echo "$VERSION" > "$PREFIX/VERSION"

mkdir -p "$OUT_DIR"
echo "[make-sdk-tarball] archiving ..."
# Sorted, with a fixed owner and mtime, so two builds of the same tree give
# byte-identical archives -- a consumer pinning by SHA256 can then tell "the
# SDK changed" from "the tarball was rebuilt".
tar --sort=name \
    --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git -C "$SDK_DIR" log -1 --format=%ct 2>/dev/null || echo 0)" \
    -czf "$OUT_DIR/$TARBALL" -C "$STAGE_ROOT" "$NAME"

echo "[make-sdk-tarball] $OUT_DIR/$TARBALL"
ls -lh "$OUT_DIR/$TARBALL" | awk '{print "                   " $5}'
