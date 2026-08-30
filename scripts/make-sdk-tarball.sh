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
# the only thing missing was an archive of it. Building it here rather than
# inline in release.yml means the release and the test run the SAME code; the
# host-tools tarball is assembled inline in the workflow and is exactly the
# kind of thing that then has no local reproduction.
#
# ONE ARCHIVE, BOTH TARGET ARCHES. The name carries the HOST platform
# (linux-x86_64), because that is what constrains who can run it: the bundled
# cross toolchains are x86_64-hosted. §5.3 measured a per-target split as
# saving ~20 MB for the cost of three packages and three smoke tests, and
# leaned against it on its own merits.
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

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)    OUT_DIR="${2:?--out needs a directory}"; shift 2 ;;
        --out=*)  OUT_DIR="${1#--out=}"; shift ;;
        --arch)   ARCH="${2:?--arch needs a value}"; shift 2 ;;
        --arch=*) ARCH="${1#--arch=}"; shift ;;
        -h|--help)
            sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "ERROR: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

VERSION="$(cat "$SDK_DIR/VERSION")"
HOST="linux-$(uname -m)"
NAME="axl-sdk-${VERSION}"
TARBALL="${NAME}-${HOST}.tar.gz"

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
