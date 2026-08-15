#!/bin/bash
# Install a bare-metal C++ cross toolchain used by axl-c++ to compile C++
# for UEFI. Replaces the AArch64-only install-arm-toolchain.sh; the two
# arches now share one script and one version table
# (scripts/axl-toolchains.conf).
#
# Idempotent: if the pinned version is already installed at the expected
# path, the script reports it and exits 0 without touching anything.
#
# Requires sudo for the /opt extract step; the script prompts when it gets
# there. Set the matching override variable (AXL_AA64_GXX / AXL_X64_GXX) to
# use a toolchain installed elsewhere instead -- see axl-toolchains.conf.
#
# Usage: ./scripts/install-toolchain.sh [aa64|x64|all]     (default: aa64)
#
# The default is aa64, NOT all: aa64 is a download, but x64 has no prebuilt
# tarball anywhere and falls back to compiling binutils + gcc + newlib, ~40
# minutes. A bare invocation must not silently start that.

set -euo pipefail

HERE="$(cd -P "$(dirname "$0")" && pwd)"

# ONE script-level EXIT trap, not a per-function RETURN trap. A RETURN trap
# does NOT fire when `set -e` aborts the script, so a curl or sha256sum failure
# mid-download would leave a partial ~96 MB tarball in /tmp forever -- and this
# runs on every tagged release. The predecessor used EXIT and was correct.
TMPDIR_DL=""
cleanup() { [[ -n "$TMPDIR_DL" ]] && rm -rf "$TMPDIR_DL"; return 0; }
trap cleanup EXIT

# The conf lives beside this script in the source tree, and in share/axl/
# when installed from a package. Try both rather than assuming a layout.
CONF=""
for candidate in "$HERE/axl-toolchains.conf" \
                 "$HERE/../share/axl/axl-toolchains.conf"; do
    if [[ -r "$candidate" ]]; then CONF="$candidate"; break; fi
done
if [[ -z "$CONF" ]]; then
    echo "ERROR: axl-toolchains.conf not found beside $0 or in ../share/axl/" >&2
    exit 1
fi
# shellcheck source=/dev/null
. "$CONF"

usage() {
    cat <<EOF
Usage: $(basename "$0") [aa64|x64|all]

  aa64   ARM's bare-metal AArch64 GNU toolchain, $AXL_AA64_TOOLCHAIN_VERSION
         (~96 MB download from developer.arm.com)          [default]
  x64    AXL's own x86_64-elf toolchain, $AXL_X64_TOOLCHAIN_VERSION
         (BUILT FROM SOURCE, ~40 min -- no upstream publishes one)
  all    both

Installs under /opt. To use a toolchain from elsewhere, skip this script and
set AXL_AA64_GXX or AXL_X64_GXX to the g++ binary instead.
EOF
}

# --- aa64: fetch ARM's prebuilt tarball -------------------------------------
HOST="x86_64"
AA64_TARGET="aarch64-none-elf"
AA64_TARBALL="arm-gnu-toolchain-${AXL_AA64_TOOLCHAIN_VERSION}-${HOST}-${AA64_TARGET}.tar.xz"
AA64_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${AXL_AA64_TOOLCHAIN_VERSION}/binrel/${AA64_TARBALL}"
# The checksum comes from the manifest alongside the version it belongs to.
# Kept apart, a version bump changes the URL but not the sha, and the install
# fails at `sha256sum -c` with a mismatch that names neither cause.
AA64_SHA256="${AXL_AA64_TOOLCHAIN_SHA256:-}"

install_aa64() {
    local gxx="$AXL_AA64_GXX_DEFAULT"
    if [[ -x "$gxx" ]]; then
        local installed
        installed="$("$gxx" --version | head -1)"
        if [[ "$installed" == *"${AXL_AA64_TOOLCHAIN_VERSION%%.rel*}"* ]]; then
            echo "[install-toolchain] aa64 already installed: $gxx"
            echo "                    $installed"
            return 0
        fi
        echo "[install-toolchain] WARNING: existing aa64 install at" \
             "$AXL_AA64_TOOLCHAIN_DIR"
        echo "                    reports unexpected version: $installed"
        echo "                    proceeding to (re)install" \
             "$AXL_AA64_TOOLCHAIN_VERSION"
    fi

    if [[ -z "$AA64_SHA256" ]]; then
        echo "[install-toolchain] ERROR: AXL_AA64_TOOLCHAIN_SHA256 is not set" \
             "in the manifest; refusing to install an unverified tarball" >&2
        return 1
    fi

    TMPDIR_DL="$(mktemp -d -t axl-toolchain-XXXXXX)"
    local tmp="$TMPDIR_DL"

    echo "[install-toolchain] downloading $AA64_TARBALL (~96 MB) ..."
    curl -fL --progress-bar -o "$tmp/$AA64_TARBALL" "$AA64_URL"

    echo "[install-toolchain] verifying sha256 ..."
    echo "${AA64_SHA256}  $tmp/$AA64_TARBALL" | sha256sum -c -

    echo "[install-toolchain] extracting to /opt/ (requires sudo) ..."
    sudo tar -xJf "$tmp/$AA64_TARBALL" -C /opt/

    if [[ ! -x "$gxx" ]]; then
        echo "[install-toolchain] ERROR: post-install g++ not found at $gxx" >&2
        return 1
    fi
    echo "[install-toolchain] installed: $gxx"
    "$gxx" --version | head -1 | sed 's/^/                    /'
}

# --- x64: build from source; nobody publishes an x86_64-elf bare-metal GCC ---
install_x64() {
    local gxx="$AXL_X64_GXX_DEFAULT"
    if [[ -x "$gxx" ]]; then
        echo "[install-toolchain] x64 already installed: $gxx"
        "$gxx" --version | head -1 | sed 's/^/                    /'
        return 0
    fi

    local builder="$HERE/../toolchain/x86_64-elf/build-toolchain.sh"
    if [[ ! -x "$builder" ]]; then
        cat >&2 <<EOF
[install-toolchain] ERROR: no x64 toolchain at $gxx, and the builder
                    ($builder) is not present.

The x86_64-elf toolchain is not yet distributed as a prebuilt tarball -- no
upstream publishes one. Build it from an axl-sdk source checkout:

  PREFIX=$AXL_X64_TOOLCHAIN_DIR toolchain/x86_64-elf/build-toolchain.sh

or point AXL_X64_GXX at an existing x86_64-elf g++.
EOF
        return 1
    fi

    echo "[install-toolchain] no prebuilt x86_64-elf toolchain exists upstream;"
    echo "                    building from source (~40 min on 8 cores) ..."
    echo "                    target: $AXL_X64_TOOLCHAIN_DIR"
    PREFIX="$AXL_X64_TOOLCHAIN_DIR" "$builder"

    if [[ ! -x "$gxx" ]]; then
        echo "[install-toolchain] ERROR: post-build g++ not found at $gxx" >&2
        return 1
    fi
    echo "[install-toolchain] installed: $gxx"
}

case "${1:-aa64}" in
    aa64)      install_aa64 ;;
    x64)       install_x64 ;;
    all)       install_aa64; install_x64 ;;
    -h|--help) usage; exit 0 ;;
    *)         echo "ERROR: unknown target '${1}'" >&2; echo >&2; usage >&2; exit 2 ;;
esac
