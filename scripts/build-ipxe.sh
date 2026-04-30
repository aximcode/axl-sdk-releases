#!/bin/bash
# build-ipxe.sh — clone iPXE at a pinned commit and build the
# universal `bin-<arch>-efi/ipxe.efidrv` driver for one or both
# architectures.
#
# The default `bin-x86_64-efi/ipxe.efidrv` target produces a single
# UEFI Driver Model driver with iPXE's full default driver set —
# Intel (e1000 / e1000e / i219 / i225), Broadcom (BCM4401 / 5760x /
# 957454), Realtek (RTL8139 / 8169 / 8125 / 8153 USB), 3Com, AMD,
# Atheros, NSC, VIA, and USB-class ethernet (CDC-ECM / CDC-NCM /
# RNDIS / AX88179). ~1.1 MB blob, ~2.9k chip IDs on x64.
#
# This is the v0.6.0 universal NIC fallback for axl-sdk's tools
# tarball — staged under drivers/<arch>/ipxe-all.efidrv where
# axl_net_ensure_drivers picks it up via the standard candidate-list
# search.
#
# Usage:
#   ./scripts/build-ipxe.sh [--arch x64|aa64|all] [--out DIR]
#
# Outputs:
#   <OUT>/ipxe-all-x64.efidrv
#   <OUT>/ipxe-all-aa64.efidrv
#
# Prereqs:
#   - gcc, binutils, make, perl, xz / liblzma headers
#   - aarch64-linux-gnu-gcc (for --arch aa64 or all)
#   - git, curl-or-wget (for the iPXE clone)
#
# License: iPXE is licensed under GPL-2.0-or-later (with some files
# under GPL-2.0-or-later-with-OpenSSL-exception or BSD). The blob
# emitted here is unmodified iPXE binary aggregated into the tools
# tarball under "mere aggregation" (GPL-2.0 §3); axl-sdk's own code
# is not subject to GPL terms. Source for GPL compliance: pinned
# commit URL printed at end of this script.

set -euo pipefail

# ---------------------------------------------------------------------------
# Pinned iPXE commit. Bumping requires re-validating in QEMU under
# `--nic-model {e1000,e1000e,rtl8139,pcnet} --nic-no-rom` to ensure
# the build still binds via NII and DHCP+HTTP go through. See
# docs/AXL-Network-Driver-Bundle-Design.md for the validation matrix.
# ---------------------------------------------------------------------------
IPXE_COMMIT="df4eec8cfb4fda2b4bc1ce87fd101b80205c1e92"
IPXE_REMOTE="https://github.com/ipxe/ipxe.git"

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

ARCH_ARG="all"
OUTDIR="$PROJECT_ROOT/out/ipxe"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)   ARCH_ARG="$2"; shift 2 ;;
        --out)    OUTDIR="$2";   shift 2 ;;
        -h|--help)
            sed -n '3,30p' "$0"
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1 ;;
    esac
done

case "$ARCH_ARG" in
    all)  ARCHES=(x64 aa64) ;;
    x64)  ARCHES=(x64) ;;
    aa64) ARCHES=(aa64) ;;
    *)    echo "ERROR: --arch must be x64, aa64, or all (got '$ARCH_ARG')" >&2; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# Prereq checks
# ---------------------------------------------------------------------------

for tool in git make gcc perl; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: '$tool' not found in PATH" >&2
        exit 1
    fi
done
for arch in "${ARCHES[@]}"; do
    if [[ "$arch" == "aa64" ]] && ! command -v aarch64-linux-gnu-gcc &>/dev/null; then
        echo "ERROR: aarch64-linux-gnu-gcc not found (needed for --arch aa64)" >&2
        echo "  Debian/Ubuntu: apt install gcc-aarch64-linux-gnu" >&2
        echo "  Fedora/RHEL:   dnf install gcc-aarch64-linux-gnu" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Clone iPXE at pinned commit (shallow)
# ---------------------------------------------------------------------------

WORK=$(mktemp -d -t axl-ipxe-build-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

echo "Cloning iPXE @ $IPXE_COMMIT ..."
git -C "$WORK" clone --quiet "$IPXE_REMOTE" ipxe
git -C "$WORK/ipxe" checkout --quiet "$IPXE_COMMIT"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

mkdir -p "$OUTDIR"

for arch in "${ARCHES[@]}"; do
    case "$arch" in
        x64)
            target="bin-x86_64-efi/ipxe.efidrv"
            make_args=()
            label="x64"
            ;;
        aa64)
            target="bin-arm64-efi/ipxe.efidrv"
            make_args=(CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64)
            label="aa64"
            ;;
    esac
    echo "Building iPXE $label ($target) ..."
    make -C "$WORK/ipxe/src" "${make_args[@]}" -j"$(nproc)" "$target" 2>&1 \
        | grep -E '\[FINISH\]|Error|error:' | tail -5
    cp "$WORK/ipxe/src/$target" "$OUTDIR/ipxe-all-$label.efidrv"
    sz=$(stat -c %s "$OUTDIR/ipxe-all-$label.efidrv")
    sha=$(sha256sum "$OUTDIR/ipxe-all-$label.efidrv" | cut -d' ' -f1)
    printf '  ipxe-all-%s.efidrv  %d bytes  sha256=%s\n' "$label" "$sz" "$sha"
done

# ---------------------------------------------------------------------------
# GPL §3(b) — written offer for source. Print so CI logs and humans
# both see exactly which commit produced these binaries.
# ---------------------------------------------------------------------------

echo
echo "iPXE source for the binaries above:"
echo "  $IPXE_REMOTE @ $IPXE_COMMIT"
echo "  git clone $IPXE_REMOTE && git checkout $IPXE_COMMIT"
