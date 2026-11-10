#!/bin/bash
# make-release-assets.sh -- build the release archives with CPack, and the
# SHA256SUMS that names them.
#
# WHY THIS EXISTS. The three archives were three scripts' own `tar` lines over
# three hand-staged trees (make-sdk-tarball.sh, make-uefi-tools-tarball.sh,
# make-host-tools-tarball.sh). What each holds is now install() rules
# (cmake/AxlInstall.cmake, cmake/AxlPackage.cmake,
# packaging/host-tools/CMakeLists.txt) -- the same rules install.sh stages a
# prefix from -- and CPack packs them. This script is the one owner of the
# ASSET NAMES and the versioned roots inside them, which are a contract:
# consumers download by exact name and pin by SHA-256
# (AXL-Distribution-Design.md §14.1a; test-release-asset-names.sh).
#
# REPRODUCIBLE, and checked (test-release-reproducible.sh): `cpack -G
# External` stages the components and cmake/AxlPackTar.cmake packs the
# staged root with `tar --sort=name --owner=0 --group=0 --numeric-owner
# --mtime=@$SOURCE_DATE_EPOCH | gzip -n`, SOURCE_DATE_EPOCH from the commit.
# CPack's own archive writer (TGZ) is not used: it records the builder's user
# and group NAMES, and lists entries in the staging directory's readdir
# order, which on ext4 is a per-filesystem hash -- two runners, two archives
# (docs/AXL-Build-System-Design.md §10.9, §10.18).
#
# Usage:
#   make-release-assets.sh sdk [--arch all|x64|aa64] [--out DIR]
#   make-release-assets.sh uefi-tools --arch x64|aa64 [--version V] [--out DIR]
#                                     [--with-rtk RTKDIR] [--built]
#   make-release-assets.sh host-tools [--out DIR]
#   make-release-assets.sh <kind> ... --print-name   the archive's name; builds nothing
#   make-release-assets.sh assets [--version V]      every asset a release of V
#                                                    publishes, one per line, bar
#                                                    SHA256SUMS; builds nothing
#
#   sdk         axl-sdk-linux-<v>-<host>.tar.gz, root axl-sdk-<v>/: the SDK
#               prefix for both arches (--arch narrows it, for tests), with
#               the C++ runtime, documentation, licences and examples
#   uefi-tools  axl-sdk-uefi-tools-<v>-<arch>.tar.gz, root of the same name:
#               the shipped .efi tools of one arch. --version overrides <v>
#               (a test pre-release); --with-rtk RTKDIR also builds and ships
#               sshd-rtk.efi and sshd-full-rtk.efi embedding
#               RTKDIR/<arch>/RtkUsbUndiDxe.efi (Realtek's binary, never by
#               default); --built reads a tree the caller already built
#               (tools and release-drivers) instead of building it
#   deb, rpm    axl-sdk-<v>_<v>-1_amd64.deb / axl-sdk-<v>-<v>-1.x86_64.rpm: the
#               SDK tree at /opt/axl-sdk-<v> as a system package
#               (AXL-Distribution-Design.md §17.5); --arch as for sdk
#   host-tools  axl-sdk-host-tools-<v>.tar.gz, root of the same name: the
#               `axl` dispatcher and host tools; needs no toolchain
#
#   assets      THE ONE PLACE the published set is decided: the four archives,
#               install.sh and VERSION, and the .deb and .rpm unless V is a
#               prerelease (an RPM Version cannot hold a `-`, and Debian would
#               sort 5.0.0-rc1 AFTER 5.0.0). release.yml, gate.check-asset-names
#               and the tests ask this rather than restate the rule.
#
# --version V with --print-name (or assets) names the assets of release V
# without building; a build takes its version from VERSION (uefi-tools alone
# may be built under another, for a test pre-release).
#
# SHA256SUMS over a release directory is scripts/release-sums.sh's.
#
# Exit 0 on success, 1 on a failure, 2 on a usage error.
set -euo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_WRAPPER="${AXL_BUILD_WRAPPER:-$SDK_DIR/scripts/axl-build.sh}"

die()   { echo "make-release-assets: $*" >&2; exit 1; }
usage() { echo "make-release-assets: $*" >&2; echo "usage: see 'make-release-assets.sh --help'" >&2; exit 2; }

# packages_apply V -- does a release of V publish the .deb and .rpm?
packages_apply() { [[ "$1" != *-* ]]; }

# asset_file KIND V [ARCH] -- an asset's file name, without its extension.
# THE ASSET NAMES -- the contract. The SDK's file name and the versioned root
# inside it are deliberately different: the root says which SDK this is, the
# file name which HOST runs it (bin/pe-set-debug is a host binary).
asset_file() {
    case "$1" in
        sdk)        echo "axl-sdk-linux-$2-$(uname -m)" ;;
        uefi-tools) echo "axl-sdk-uefi-tools-$2-$3" ;;
        host-tools) echo "axl-sdk-host-tools-$2" ;;
        # The distributions' own file-name conventions (§17.5 P7): the
        # package axl-sdk-<v>, version <v>, release 1.
        deb)        echo "axl-sdk-$2_$2-1_amd64" ;;
        rpm)        echo "axl-sdk-$2-$2-1.x86_64" ;;
    esac
}

[[ $# -ge 1 ]] || usage "a kind is required: sdk, uefi-tools, host-tools, deb, rpm or assets"
KIND="$1"; shift
case "$KIND" in
    -h|--help) sed -n '2,58p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    sdk|uefi-tools|host-tools|deb|rpm|assets) ;;
    *) usage "unknown kind '$KIND' (sdk, uefi-tools, host-tools, deb, rpm or assets)" ;;
esac

if [[ "$KIND" == assets ]]; then
    V="$(cat "$SDK_DIR/VERSION")"
    case "${1:-}" in
        --version) V="${2:?--version needs a version}"; shift 2 ;;
    esac
    [[ $# -eq 0 ]] || usage "assets takes only --version V"
    {
        echo "$(asset_file sdk "$V").tar.gz"
        echo "$(asset_file host-tools "$V").tar.gz"
        echo "$(asset_file uefi-tools "$V" x64).tar.gz"
        echo "$(asset_file uefi-tools "$V" aa64).tar.gz"
        if packages_apply "$V"; then
            echo "$(asset_file deb "$V").deb"
            echo "$(asset_file rpm "$V").rpm"
        fi
        echo install.sh
        echo VERSION
    } | LC_ALL=C sort
    exit 0
fi

ARCH="" V="" OUT_DIR="$SDK_DIR/dist" PRINT_NAME=0 RTK_DIR="" BUILT=0
[[ "$KIND" == sdk || "$KIND" == deb || "$KIND" == rpm ]] && ARCH=all
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)       ARCH="${2:?--arch needs a value}"; shift 2 ;;
        --version)    V="${2:?--version needs a version}"; shift 2 ;;
        --out)        OUT_DIR="${2:?--out needs a directory}"; shift 2 ;;
        --print-name) PRINT_NAME=1; shift ;;
        --with-rtk)   RTK_DIR="${2:?--with-rtk needs the directory holding <arch>/RtkUsbUndiDxe.efi}"; shift 2 ;;
        --built)      BUILT=1; shift ;;
        *) usage "unknown argument '$1'" ;;
    esac
done
case "$KIND:$ARCH" in
    sdk:all|sdk:x64|sdk:aa64|deb:all|deb:x64|deb:aa64|rpm:all|rpm:x64|rpm:aa64) ;;
    uefi-tools:x64|uefi-tools:aa64|host-tools:) ;;
    uefi-tools:*) usage "uefi-tools needs --arch x64 or aa64" ;;
    host-tools:*) usage "host-tools takes no --arch" ;;
    *)            usage "sdk takes --arch all, x64 or aa64" ;;
esac
[[ -z "$V" || "$KIND" == uefi-tools ]] || (( PRINT_NAME )) \
    || usage "--version applies to uefi-tools (and to --print-name)"
[[ -z "$RTK_DIR" && "$BUILT" == 0 || "$KIND" == uefi-tools ]] \
    || usage "--with-rtk and --built apply to uefi-tools"

VERSION="$(cat "$SDK_DIR/VERSION")"
# --version names another release's assets (--print-name), or builds the
# uefi-tools archive under it; everything else builds as VERSION.
NAME_V="${V:-$VERSION}"
[[ -n "$V" ]] || V="$VERSION"

FILE="$(asset_file "$KIND" "$NAME_V" "$ARCH")"
case "$KIND" in
    sdk)            ROOT="axl-sdk-${NAME_V}" ;;
    uefi-tools)     ROOT="$FILE" ;;
    host-tools)     ROOT="$FILE" ;;
    deb|rpm)        ROOT="opt/axl-sdk-${NAME_V}" ;;
esac
# A PRERELEASE gets no package (packages_apply, above).
if [[ "$KIND" == deb || "$KIND" == rpm ]] && ! packages_apply "$NAME_V"; then
    usage "no .deb/.rpm for a prerelease version ($NAME_V)"
fi
case "$KIND" in
    deb) EXT=deb ;;
    rpm) EXT=rpm ;;
    *)   EXT=tar.gz ;;
esac
if (( PRINT_NAME )); then
    echo "${FILE}.${EXT}"
    exit 0
fi

# shellcheck source=./axl-common.sh
source "$SDK_DIR/scripts/axl-common.sh"
# shellcheck source=./axl-cmake-pin.sh
source "$SDK_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

# CPack's working tree (_CPack_Packages) and the packaging projects' build
# trees go to a scratch directory, never --out (release.yml uploads what
# --out holds) and never the source tree (two runs must not share one).
WORK="$(mktemp -d -t axl-cpack.XXXXXXXX)"
axl_trap_cleanup 'rm -rf "$WORK"'

# Every mtime in the archive is the commit's, so a rebuild of one commit
# gives the same bytes -- and the SDK's share/axl/build-date is that day.
#
# git's REFUSAL is not "no checkout": a container running as root over a
# checkout another uid owns gets `fatal: detected dubious ownership` (exit
# 128), and this once said "not a git checkout" for exactly that, because it
# threw git's stderr away (CI, 2026-11-10). So git's own words are kept, a
# missing repository is told apart from every other failure, and an answer
# that is not a number -- git's or the caller's -- is not taken for one
# (test-release-assets-commit-time.sh).
if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
    git_rc=0
    SOURCE_DATE_EPOCH="$(LC_ALL=C git -C "$SDK_DIR" log -1 --format=%ct 2>"$WORK/git.err")" || git_rc=$?
    git_msg="$(cat "$WORK/git.err")"
    if (( git_rc != 0 )); then
        [[ "$git_msg" == *"not a git repository"* ]] \
            && die "not a git checkout ($SDK_DIR): set SOURCE_DATE_EPOCH to the time the archive should carry"
        {
            echo "make-release-assets: git could not read the commit time of $SDK_DIR (exit $git_rc):"
            sed '/^$/d; s/^/    /' <<< "$git_msg"
        } >&2
        [[ "$git_msg" == *"dubious ownership"* ]] \
            && die "the checkout belongs to another user (a container running as root?): trust it first with git config --global --add safe.directory \"$SDK_DIR\", or set SOURCE_DATE_EPOCH"
        die "fix git, or set SOURCE_DATE_EPOCH to the time the archive should carry"
    fi
    [[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] \
        || die "git log answered '$SOURCE_DATE_EPOCH', not a commit time: set SOURCE_DATE_EPOCH to the time the archive should carry"
elif [[ ! "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]]; then
    die "SOURCE_DATE_EPOCH is '$SOURCE_DATE_EPOCH', not a time in seconds"
fi
export SOURCE_DATE_EPOCH

# tree_of ARCH -- the RELEASE tree, ASKED of the build wrapper, never composed.
tree_of() {
    local p
    p="$("$BUILD_WRAPPER" --print-prefix ARCH="$1" BUILD=RELEASE)" && [[ -n "$p" ]] \
        || die "axl-build.sh --print-prefix ARCH=$1 failed"
    # Relative to the source tree as axl-build.sh answers it; a wrapper
    # standing in for it (AXL_BUILD_WRAPPER) may answer absolute.
    [[ "$p" == /* ]] && echo "$p" || echo "$SDK_DIR/$p"
}

# configure_packaging NAME ARGS... -- configure packaging/NAME (a project
# with no compiler) into $WORK/NAME; echo its build tree.
configure_packaging() {
    local name="$1"; shift
    if ! cmake -S "$SDK_DIR/packaging/$name" -B "$WORK/$name" -G Ninja "$@" \
            > "$WORK/configure-$name.log" 2>&1; then
        cat "$WORK/configure-$name.log" >&2
        die "configuring packaging/$name failed (above)"
    fi
    echo "$WORK/$name"
}

# relay -- what the install rules said about the content (a tool not
# shipped, a tool missing): the notes file the uefi-tools rules write, one
# line each, on stderr.
relay() {
    local f
    for f in "$WORK"/*/uefi-tools-notes.txt; do
        [[ -f "$f" ]] || continue
        sed 's/^/make-release-assets: /' "$f" >&2
    done
}

PROJECTS=()
CONFIG=""
case "$KIND" in
    sdk|deb|rpm)
        if [[ "$ARCH" == all ]]; then ARCHES=(x64 aa64); else ARCHES=("$ARCH"); fi
        last="${ARCHES[${#ARCHES[@]}-1]}"
        for a in "${ARCHES[@]}"; do
            t="$(tree_of "$a")"
            echo "make-release-assets: building the $a SDK in $t"
            # The tree's build lock, as install.sh takes it: held until cpack
            # has read the tree (fds 8 and 9, one per arch).
            # A fresh clone has no build/ yet (release.yml's build-sdk job
            # builds nothing before this), and the lock lives beside the tree.
            mkdir -p "$(dirname "$t")"
            if command -v flock >/dev/null 2>&1; then
                if [[ "$a" == x64 ]]; then
                    exec 8>"$t.buildlock"; flock 8
                else
                    exec 9>"$t.buildlock"; flock 9
                fi
            fi
            "$BUILD_WRAPPER" ARCH="$a" BUILD=RELEASE sdk >/dev/null \
                || die "the $a RELEASE build failed (scripts/axl-build.sh ARCH=$a BUILD=RELEASE sdk)"
            PROJECTS+=("$t" axl sdk_arch / "$t" axl sdk_cxxrt /)
            if [[ "$a" == "$last" ]]; then
                PROJECTS+=("$t" axl sdk_host / "$t" axl sdk_common / "$t" axl sdk_doc /)
                CONFIG="$t/CPackConfig-release.cmake"
                if [[ "$KIND" != sdk ]]; then
                    # The package's own receipt and profile.d script, and its
                    # config (name, dependencies, maintainer scripts).
                    PROJECTS+=("$t" axl "pkg_$KIND" /)
                    CONFIG="$t/CPackConfig-package.cmake"
                fi
            fi
        done
        ;;
    uefi-tools)
        t="$(tree_of "$ARCH")"
        # Said, so a caller can check which tree this was.
        echo "make-release-assets: staging from $t"
        if [[ -n "$RTK_DIR" ]]; then
            drv="$RTK_DIR/$ARCH/RtkUsbUndiDxe.efi"
            [[ -f "$drv" ]] || { echo "make-release-assets: --with-rtk: no $drv" >&2; exit 2; }
            drv="$(cd "$(dirname "$drv")" && pwd)/RtkUsbUndiDxe.efi"
            # Into a directory of its own, never the shared tools/: a release's
            # RTK build left there was once tested as if it were a tool.
            export AXL_RTK_OUT="$t/rtk-release"
            "$BUILD_WRAPPER" ARCH="$ARCH" BUILD=RELEASE SSHD_RTK_OUT="$AXL_RTK_OUT" \
                RTK_USB_UNDI="$drv" sshd-rtk sshd-full-rtk >/dev/null \
                || die "the sshd-rtk build failed"
        else
            unset AXL_RTK_OUT
        fi
        if (( ! BUILT )); then
            "$BUILD_WRAPPER" ARCH="$ARCH" BUILD=RELEASE tools release-drivers >/dev/null \
                || die "the $ARCH RELEASE build failed (tools release-drivers)"
        fi
        export AXL_RELEASE_VERSION="$V"
        # The images are $t's; the rules are packaging/uefi-tools', which
        # read them from the tree they are given.
        p="$(configure_packaging uefi-tools -DAXL_ARCH="$ARCH" -DAXL_IMAGE_TREE="$t")" || exit 1
        PROJECTS=("$p" axl_uefi_tools uefi_tools /)
        CONFIG="$p/CPackConfig-release.cmake"
        ;;
    host-tools)
        # Its own project, with no compiler: no toolchain needed.
        p="$(configure_packaging host-tools)" || exit 1
        PROJECTS=("$p" axl_host_tools host_tools /)
        CONFIG="$p/CPackConfig-release.cmake"
        ;;
esac
[[ -f "$CONFIG" ]] || die "$CONFIG is missing -- the tree predates cmake/AxlPackage.cmake; reconfigure it (scripts/axl-build.sh rebuilds it)"

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
projects="$(IFS=';'; echo "${PROJECTS[*]}")"
# A FIXED umask: CPack creates the staged directories with the builder's
# (CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS does not reach them), and tar
# records directory modes as staged, so a builder at 077 would ship
# drwx------ directories -- a different archive, and one a non-root user
# cannot read once root extracts it (test-release-reproducible.sh builds at
# 077 to hold this).
umask 022
echo "make-release-assets: packing ${FILE}.${EXT} (SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH)"
case "$KIND" in
    # The .deb through dpkg-deb, not CPack's DEB writer (whose Installed-Size
    # depends on the filesystem): packaging/system/AxlPackDeb.cmake.
    deb) GEN=(-G External -D CPACK_EXTERNAL_ENABLE_STAGING=ON
              -D "CPACK_EXTERNAL_PACKAGE_SCRIPT=$SDK_DIR/packaging/system/AxlPackDeb.cmake"
              -D "CPACK_PACKAGE_FILE_NAME=$FILE" -D "CPACK_DEBIAN_FILE_NAME=$FILE.deb") ;;
    rpm) GEN=(-G RPM -D "CPACK_PACKAGE_FILE_NAME=$FILE" -D "CPACK_RPM_FILE_NAME=$FILE.rpm") ;;
    *)   GEN=(-D "CPACK_PACKAGE_FILE_NAME=$FILE" -D "CPACK_PACKAGING_INSTALL_PREFIX=/$ROOT") ;;
esac
if ! cpack --config "$CONFIG" -B "$WORK" "${GEN[@]}" \
        -D "CPACK_INSTALL_CMAKE_PROJECTS=$projects" > "$WORK/cpack.log" 2>&1; then
    cat "$WORK/cpack.log" >&2
    relay
    die "cpack failed (above)"
fi
relay
[[ -f "$WORK/$FILE.$EXT" ]] || die "cpack wrote no $FILE.$EXT"
if [[ "$EXT" != tar.gz ]]; then
    mv -f "$WORK/$FILE.$EXT" "$OUT_DIR/$FILE.$EXT"
    echo "make-release-assets: wrote $OUT_DIR/$FILE.$EXT"
    exit 0
fi

# ONE VERSIONED ROOT: an archive that unpacked its entries into the caller's
# directory scattered over whatever was there (§14.1c). Listed once to a
# file: `tar tzf | grep -q` under pipefail fails on a match.
tar tzf "$WORK/$FILE.tar.gz" > "$WORK/list"
if [[ "$(awk -F/ '{print $1}' "$WORK/list" | sort -u)" != "$ROOT" ]]; then
    die "${FILE}.tar.gz does not have exactly one root named $ROOT"
fi
mv -f "$WORK/$FILE.tar.gz" "$OUT_DIR/$FILE.tar.gz"
echo "make-release-assets: wrote $OUT_DIR/$FILE.tar.gz ($(wc -l < "$WORK/list") entries)"
