#!/bin/bash
# Install the ARM bare-metal AArch64 GNU toolchain used by axl-c++
# for cross-compiling C++ to UEFI.  Pinned to the version Dell ePSA
# uses (delldiags/source/src/EPSA/Include.mak) so axl-sdk-built C++
# and Dell-built C++ share an ABI.
#
# Idempotent: if the pinned version is already installed at the
# expected /opt path, the script exits successfully without touching
# anything.
#
# Requires sudo for the /opt extract step; the script prompts when
# it reaches that point.
#
# Usage: ./scripts/install-arm-toolchain.sh

set -euo pipefail

# --- Pinned version (must match docs/ROADMAP.md axlmm spec) ----------
VERSION="14.3.rel1"
HOST="x86_64"
TARGET="aarch64-none-elf"
TARBALL="arm-gnu-toolchain-${VERSION}-${HOST}-${TARGET}.tar.xz"
URL="https://developer.arm.com/-/media/Files/downloads/gnu/${VERSION}/binrel/${TARBALL}"
SHA256="ebaf2d47f2e7f7b645864c5c8cf839e526daed83a2e675a3525d03f5ba3d2be9"
INSTALL_PARENT="/opt"
INSTALL_DIR="${INSTALL_PARENT}/arm-gnu-toolchain-${VERSION}-${HOST}-${TARGET}"
GXX="${INSTALL_DIR}/bin/${TARGET}-g++"

# --- Idempotency check ----------------------------------------------
if [[ -x "$GXX" ]]; then
    INSTALLED_VER="$("$GXX" --version | head -1)"
    if [[ "$INSTALLED_VER" == *"14.3"* ]]; then
        echo "[install-arm-toolchain] already installed: $GXX"
        echo "                        $INSTALLED_VER"
        exit 0
    fi
    echo "[install-arm-toolchain] WARNING: existing install at $INSTALL_DIR"
    echo "                        reports unexpected version: $INSTALLED_VER"
    echo "                        proceeding to (re)install $VERSION"
fi

# --- Download -------------------------------------------------------
TMP="$(mktemp -d -t arm-toolchain-XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

echo "[install-arm-toolchain] downloading $TARBALL (~96 MB) ..."
curl -fL --progress-bar -o "$TMP/$TARBALL" "$URL"

echo "[install-arm-toolchain] verifying sha256 ..."
echo "${SHA256}  $TMP/$TARBALL" | sha256sum -c -

# --- Extract --------------------------------------------------------
echo "[install-arm-toolchain] extracting to $INSTALL_PARENT/ (requires sudo) ..."
sudo tar -xJf "$TMP/$TARBALL" -C "$INSTALL_PARENT/"

# --- Verify ---------------------------------------------------------
if [[ ! -x "$GXX" ]]; then
    echo "[install-arm-toolchain] ERROR: post-install g++ not found at $GXX" >&2
    exit 1
fi
INSTALLED_VER="$("$GXX" --version | head -1)"
echo "[install-arm-toolchain] installed: $GXX"
echo "                        $INSTALLED_VER"
echo
echo "Add to PATH (optional — axl-cc finds it at the conventional /opt path):"
echo "  export PATH=\"$INSTALL_DIR/bin:\$PATH\""
