#!/bin/bash
# Install a bare-metal C++ cross toolchain used by axl-c++ to compile C++
# for UEFI. Replaces the AArch64-only install-arm-toolchain.sh; the two
# arches now share one script and one version table
# (scripts/axl-toolchains.conf).
#
# Idempotent: if the pinned version is already installed at the expected
# path, the script reports it and exits 0 without touching anything.
#
# Uses sudo for the /opt extract step when not already root; the script prompts when it gets
# there. Set the matching override variable (AXL_AA64_GXX / AXL_X64_GXX) to
# use a toolchain installed elsewhere instead -- see axl-toolchains.conf.
#
# Usage: ./scripts/install-toolchain.sh [aa64|x64|all]     (default: aa64)
#
# The default is aa64, NOT all. Both are downloads now -- x64 gained a published
# tarball with the -axl toolchain release -- but x64 still FALLS BACK to
# compiling binutils + gcc + newlib (~40 min) when the download fails and the
# source tree is present. A bare invocation must not silently start that.

set -euo pipefail

HERE="$(cd -P "$(dirname "$0")" && pwd)"
# axl-common.sh sits beside this script in a checkout, but this file is the
# one host tool staged into bin/ rather than libexec/axl/, so in a staged
# prefix the neighbour is one directory over. One path cannot serve both.
for _axl_common in "$HERE/axl-common.sh" "$HERE/../libexec/axl/axl-common.sh"; do
    [[ -r "$_axl_common" ]] && { source "$_axl_common"; break; }
done

# Answered before anything else parses argv, so `--version` never has to
# survive a positional-hungry option loop. See axl-common.sh.
axl_handle_version "axl-install-toolchain" "$@" && exit 0

# ONE script-level EXIT trap, not a per-function RETURN trap. A RETURN trap
# does NOT fire when `set -e` aborts the script, so a curl or sha256sum failure
# mid-download would leave a partial ~96 MB tarball in /tmp forever -- and this
# runs on every tagged release. The predecessor used EXIT and was correct.
#
# An ARRAY, not a scalar. `install-toolchain.sh all` downloads twice, and a
# scalar means the second mktemp overwrites the first -- so the ~96 MB aa64
# tree survives the run. That was latent while x64 built from source and took
# no download path at all; it stopped being latent when x64 gained a tarball.
TMPDIRS_DL=()
new_tmpdir_dl() {
    local d
    d="$(mktemp -d -t axl-toolchain-XXXXXX)"
    TMPDIRS_DL+=("$d")
    echo "$d"
}
cleanup() {
    local d
    for d in ${TMPDIRS_DL[@]+"${TMPDIRS_DL[@]}"}; do
        rm -rf "$d"
    done
    return 0
}
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
# Elevate only when we are not already root. A CI container runs as root and
# frequently has no `sudo` installed at all, so calling it unconditionally
# fails with "command not found" on a machine that needed no elevation --
# which is how the lint job first met this script.
#
# When elevation IS needed and sudo is absent, say so. The packages
# deliberately do not Depend on sudo -- it would be pulled onto every root-only
# container that skips this branch entirely -- so the miss is reachable, and
# `sudo: command not found` names the wrong problem after a 96 MB download has
# already succeeded.
# Run $@ with whatever privilege the DESTINATION needs -- not with whatever
# privilege /opt would have needed. Once the prefix is a parameter, "extracting
# to /opt needs root" is wrong twice over: it is the wrong path to tell a
# reader who chose another one, and it escalates for a directory the user
# already owns. A prefix under $HOME needs no sudo at all, which is the whole
# point of --prefix.
need_root_for() {
    local dest="$1"
    # Walk up to the nearest existing ancestor: the leaf usually does not exist
    # yet, and an unwritable /opt is discovered at /opt, not at /opt/foo.
    while [[ -n "$dest" && ! -e "$dest" ]]; do dest="$(dirname "$dest")"; done
    [[ ! -w "$dest" ]]
}

as_root_for() {
    local dest="$1"; shift
    if ! need_root_for "$dest"; then
        "$@"
        return
    fi
    if [[ "$(id -u)" -eq 0 ]]; then
        "$@"
        return
    fi
    if ! command -v sudo >/dev/null 2>&1; then
        echo "[install-toolchain] ERROR: cannot write $dest, and this is not" >&2
        echo "                    root and has no sudo." >&2
        echo "                    Re-run as root, install sudo, or pick a" >&2
        echo "                    prefix you own:  --prefix \$HOME/.local/opt" >&2
        return 1
    fi
    sudo "$@"
}

# shellcheck source=/dev/null
. "$CONF"

# Where toolchains get installed. /opt to match the manifest's defaults; any
# other value relocates the install AND is echoed back as the AXL_<ARCH>_*
# settings needed to use it, since the manifest's defaults still name /opt.
#
# The per-arch directory keeps its manifest NAME under the chosen root, and the
# compiler path is the manifest's own with that directory's prefix rewritten --
# so "the directory prefixes the compiler", which check-toolchain-conf asserts
# about the manifest, stays true of what is installed.
INSTALL_ROOT="${INSTALL_ROOT:-/opt}"

relocated_dir() {   # <manifest TOOLCHAIN_DIR> -> that name under INSTALL_ROOT
    printf '%s/%s' "${INSTALL_ROOT%/}" "$(basename "$1")"
}
relocated_tool() {  # <manifest tool path> <manifest dir> <new dir>
    printf '%s' "${1/#$2/$3}"
}

# A toolchain outside /opt is invisible until the AXL_<ARCH>_* locators point
# at it: the manifest's defaults still name /opt, and axl-cc, the Makefile and
# install.sh all resolve ${VAR:-$VAR_DEFAULT}. The installer is the only party
# that knows the prefix that was chosen, so it is the only one that can say
# this -- and succeeding silently would leave the user with ~96 MB on disk and
# a build that still cannot find a compiler.
print_env() {   # <arch> <new dir> <manifest dir> <gcc> <gxx> <binutils>
    local arch="$1" dir="$2" mdir="$3" gcc="$4" gxx="$5" bin="$6"
    [[ "$dir" != "$mdir" ]] || return 0
    local up; up="$(echo "$arch" | tr "[:lower:]" "[:upper:]")"
    echo ""
    echo "[install-toolchain] $arch is NOT in the manifest default location, so"
    echo "                    set these before building (add to your shell rc):"
    echo ""
    echo "  export AXL_${up}_TOOLCHAIN_DIR=$dir"
    echo "  export AXL_${up}_GCC=$(relocated_tool "$gcc" "$mdir" "$dir")"
    echo "  export AXL_${up}_GXX=$(relocated_tool "$gxx" "$mdir" "$dir")"
    echo "  export AXL_${up}_BINUTILS_PREFIX=$(relocated_tool "$bin" "$mdir" "$dir")"
    echo ""
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [aa64|x64|all] [--prefix DIR]

  aa64   ARM's bare-metal AArch64 GNU toolchain, $AXL_AA64_TOOLCHAIN_VERSION
         (~96 MB download from developer.arm.com)          [default]
  x64    AXL's own x86_64-elf toolchain, $AXL_X64_TOOLCHAIN_VERSION
         (~55 MB download; no upstream publishes one, so AXL builds and
         hosts it, and this falls back to a ~40 min source build)
  all    both

  --prefix DIR
         Install under DIR instead of /opt. Root is requested only when the
         destination actually needs it, so a prefix you own (say
         \$HOME/.local/opt) needs no sudo at all. The run then prints the
         AXL_<ARCH>_* settings to export, because the manifest's defaults
         still name /opt.

Installs under /opt by default.
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
# AXL_INSECURE_FETCH=1 -- skip TLS verification for these downloads.
#
# SOUND HERE, unlike almost anywhere else, and the reason is worth stating: the
# expected SHA256 comes from scripts/axl-toolchains.conf, which SHIPS IN THE
# SDK. It is pre-shared and out-of-band, so it does not travel with the
# artifact and an interceptor cannot forge a tarball to match it. TLS is
# authenticating a channel; the pinned hash authenticates the BYTES, which is
# what we actually care about for a fixed known artifact.
#
# It exists because Dell (and others) intercept HTTPS org-wide with a CA a
# fresh machine does not trust, so this fetch dies at TLS on a host that is
# otherwise perfectly able to install. Measured: against a store that does not
# trust the presented cert, plain curl gets http=000 and `curl -k` gets a 302,
# and the proxy relays the file bytes unchanged so the pinned hash still
# matches -- and would still fail if it did not.
#
# packaging/install.sh honours the same variable but its guarantee is WEAKER:
# it verifies against a SHA256SUMS it fetches over the same connection. Its
# warning says so.
INSECURE_CURL=()
if [[ "${AXL_INSECURE_FETCH:-0}" == "1" ]]; then
    INSECURE_CURL=(-k)
    echo "[install-toolchain] AXL_INSECURE_FETCH=1 -- TLS verification OFF." >&2
    echo "[install-toolchain]   The tarball is still verified against the SHA256" >&2
    echo "[install-toolchain]   pinned in axl-toolchains.conf, which ships with the" >&2
    echo "[install-toolchain]   SDK rather than travelling with the download." >&2
fi

# Name BOTH ways out. `set -e` used to abort on the failed curl with nothing but
# curl's own message, and the only guidance a user saw ("install the corporate
# CA") lived in a downstream consumer's wrapper -- so a direct SDK user got
# none at all.
fetch_hint() {
    echo "" >&2
    echo "[install-toolchain] download failed. If this is a TLS/certificate error," >&2
    echo "  the usual cause is a corporate proxy intercepting HTTPS. Two ways out:" >&2
    echo "    1. install your organisation's CA chain (per machine), or" >&2
    echo "    2. AXL_INSECURE_FETCH=1 $0 $* " >&2
    echo "       -- skips TLS verification. The tarball is still checked against" >&2
    echo "       the SHA256 pinned in scripts/axl-toolchains.conf, which ships" >&2
    echo "       with the SDK, so tampering is still caught." >&2
}

AA64_SHA256="${AXL_AA64_TOOLCHAIN_SHA256:-}"

install_aa64() {
    local dir gxx
    dir="$(relocated_dir "$AXL_AA64_TOOLCHAIN_DIR")"
    gxx="$(relocated_tool "$AXL_AA64_GXX_DEFAULT" "$AXL_AA64_TOOLCHAIN_DIR" "$dir")"
    if [[ -x "$gxx" ]]; then
        local installed
        installed="$("$gxx" --version | head -1)"
        if [[ "$installed" == *"${AXL_AA64_TOOLCHAIN_VERSION%%.rel*}"* ]]; then
            echo "[install-toolchain] aa64 already installed: $gxx"
            echo "                    $installed"
            return 0
        fi
        echo "[install-toolchain] WARNING: existing aa64 install at" \
             "$dir"
        echo "                    reports unexpected version: $installed"
        echo "                    proceeding to (re)install" \
             "$AXL_AA64_TOOLCHAIN_VERSION"
    fi

    if [[ -z "$AA64_SHA256" ]]; then
        echo "[install-toolchain] ERROR: AXL_AA64_TOOLCHAIN_SHA256 is not set" \
             "in the manifest; refusing to install an unverified tarball" >&2
        return 1
    fi

    local tmp
    tmp="$(new_tmpdir_dl)"

    echo "[install-toolchain] downloading $AA64_TARBALL (~96 MB) ..."
    curl "${INSECURE_CURL[@]}" -fL --progress-bar -o "$tmp/$AA64_TARBALL" "$AA64_URL" \
        || { fetch_hint "$@"; return 1; }

    echo "[install-toolchain] verifying sha256 ..."
    echo "${AA64_SHA256}  $tmp/$AA64_TARBALL" | sha256sum -c -

    echo "[install-toolchain] extracting to $INSTALL_ROOT/ ..."
    as_root_for "$INSTALL_ROOT" mkdir -p "$INSTALL_ROOT" || return 1
    as_root_for "$INSTALL_ROOT" tar -xJf "$tmp/$AA64_TARBALL" -C "$INSTALL_ROOT" || return 1

    if [[ ! -x "$gxx" ]]; then
        echo "[install-toolchain] ERROR: post-install g++ not found at $gxx" >&2
        return 1
    fi
    echo "[install-toolchain] installed: $gxx"
    "$gxx" --version | head -1 | sed 's/^/                    /'
    print_env aa64 "$dir" "$AXL_AA64_TOOLCHAIN_DIR" \
        "$AXL_AA64_GCC_DEFAULT" "$AXL_AA64_GXX_DEFAULT" \
        "$AXL_AA64_BINUTILS_PREFIX_DEFAULT"
}

# --- x64: build from source; nobody publishes an x86_64-elf bare-metal GCC ---
install_x64() {
    local dir gxx
    dir="$(relocated_dir "$AXL_X64_TOOLCHAIN_DIR")"
    gxx="$(relocated_tool "$AXL_X64_GXX_DEFAULT" "$AXL_X64_TOOLCHAIN_DIR" "$dir")"
    if [[ -x "$gxx" ]]; then
        echo "[install-toolchain] x64 already installed: $gxx"
        "$gxx" --version | head -1 | sed 's/^/                    /'
        return 0
    fi

    local builder="$HERE/../toolchain/x86_64-elf/build-toolchain.sh"
    # PREFERRED: the prebuilt tarball. No upstream publishes an x86_64-elf
    # bare-metal GCC, so AXL builds one and publishes it itself -- 55 MB here
    # against ~40 minutes of compiling, which is the difference between a
    # usable CI job and an unusable one. Same download-and-verify shape as
    # aa64; the URL and sha live in axl-toolchains.conf beside the version.
    local url="${AXL_X64_TOOLCHAIN_URL:-}"
    local sha="${AXL_X64_TOOLCHAIN_SHA256:-}"
    if [[ -n "$url" && -n "$sha" && "$sha" != "PENDING_UPLOAD" ]]; then
        local tmp
        tmp="$(new_tmpdir_dl)"
        local tarball="${url##*/}"

        echo "[install-toolchain] downloading $tarball (~55 MB) ..."
        if curl "${INSECURE_CURL[@]}" -fL --progress-bar -o "$tmp/$tarball" "$url"; then
            echo "[install-toolchain] verifying sha256 ..."
            if echo "${sha}  $tmp/$tarball" | sha256sum -c -; then
                echo "[install-toolchain] extracting to /opt/ ..."
                as_root_for "$INSTALL_ROOT" mkdir -p "$INSTALL_ROOT" \
                    && as_root_for "$INSTALL_ROOT" tar -xJf "$tmp/$tarball" -C "$INSTALL_ROOT"
                if [[ -x "$gxx" ]]; then
                    echo "[install-toolchain] installed: $gxx"
                    return 0
                fi
                echo "[install-toolchain] WARNING: post-extract g++ missing at" \
                     "$gxx; falling back to a source build" >&2
            else
                # A checksum mismatch is NOT something to build around
                # silently: it means the manifest and the artifact disagree,
                # and the source build would hide that until the next bump.
                echo "[install-toolchain] ERROR: sha256 mismatch for $tarball" >&2
                echo "                    manifest and published artifact disagree" >&2
                return 1
            fi
        else
            # The fallback here is a ~30 MINUTE source build of GCC, so the
            # hint has to come BEFORE it, not after. A TLS failure that silently
            # turns into a half-hour compile is the worst version of this bug.
            fetch_hint "$@"
            echo "[install-toolchain] download failed; falling back to a" \
                 "source build (this takes ~30 minutes -- Ctrl-C if one of" \
                 "the two fixes above is what you wanted)" >&2
        fi
    fi

    if [[ ! -x "$builder" ]]; then
        cat >&2 <<EOF
[install-toolchain] ERROR: no x64 toolchain at $gxx, no usable prebuilt
                    tarball, and the builder ($builder) is not present.

Build it from an axl-sdk source checkout:

  PREFIX=$AXL_X64_TOOLCHAIN_DIR toolchain/x86_64-elf/build-toolchain.sh

or point AXL_X64_GCC / AXL_X64_GXX at an existing x86_64-elf toolchain.
EOF
        return 1
    fi

    echo "[install-toolchain] building x86_64-elf from source (~40 min on 8" \
         "cores) ..."
    echo "                    target: $dir"
    PREFIX="$dir" "$builder"

    if [[ ! -x "$gxx" ]]; then
        echo "[install-toolchain] ERROR: post-build g++ not found at $gxx" >&2
        return 1
    fi
    echo "[install-toolchain] installed: $gxx"
    print_env x64 "$dir" "$AXL_X64_TOOLCHAIN_DIR" \
        "$AXL_X64_GCC_DEFAULT" "$AXL_X64_GXX_DEFAULT" \
        "$AXL_X64_BINUTILS_PREFIX_DEFAULT"
}

TARGET_SEL=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)   INSTALL_ROOT="${2:?--prefix needs a directory}"; shift 2 ;;
        --prefix=*) INSTALL_ROOT="${1#--prefix=}"; shift ;;
        -h|--help)  usage; exit 0 ;;
        aa64|x64|all) TARGET_SEL="$1"; shift ;;
        *) echo "ERROR: unknown argument '$1'" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
done

case "${TARGET_SEL:-aa64}" in
    aa64) install_aa64 ;;
    x64)  install_x64 ;;
    all)  install_aa64; install_x64 ;;
esac
