#!/bin/bash
# Build AXL and package the SDK for standalone use.
#
# Usage: ./scripts/install.sh [OPTIONS]
#
# Options:
#   --prefix DIR           Install location (default: ./stage)
#   --arch ARCH            Build only this arch: x64, aa64, or all (default: all)
#
# Requires: gcc, ld, ar, objcopy (GCC toolchain)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

# Defaults
PREFIX="$SDK_DIR/stage"
LIBAXL_DIR="$SDK_DIR"
BUILD_ARCHS="all"
# C++ support mode: "auto" (default — build if toolchain present, skip
# silently otherwise), "require" (--cpp: fail loud if missing), "skip"
# (--no-cpp: don't build even if present).
CPP_MODE="auto"
PRINT_BUILD_LOCK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)      PREFIX="$2"; shift 2 ;;
        --arch)        BUILD_ARCHS="$2"; shift 2 ;;
        --cpp)         CPP_MODE="require"; shift ;;
        --no-cpp)      CPP_MODE="skip"; shift ;;
        # Print the lock this invocation would take around its build, then
        # exit. Exists so a caller can reason about the serialisation without
        # re-deriving the path -- a second spelling of the derivation is a
        # thing that drifts, and the test for the lock would otherwise have to
        # own one.
        --print-build-lock) PRINT_BUILD_LOCK=1; shift ;;
        -h|--help)
            cat <<HELP
Usage: $0 [--prefix DIR] [--arch x64|aa64|all] [--cpp|--no-cpp]

C++ support is built automatically when the C++ toolchain is
present — the bare-metal cross at /opt on BOTH arches, whose paths
live in scripts/axl-toolchains.conf. Otherwise the install is
C-only — no warning, no error.

  --cpp       Require C++ support; fail loud if toolchain missing.
              Useful in CI / scripted setups where pure-C is wrong.
  --no-cpp    Skip C++ support even if the toolchain is present.
              Useful for minimal C-only installs.

To acquire the C++ cross toolchains (installed under /opt):
  ./scripts/install-toolchain.sh [aa64|x64|all]
HELP
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Build and package
# ---------------------------------------------------------------------------

source "$LIBAXL_DIR/scripts/axl-common.sh"

# ---------------------------------------------------------------------------
# Content-preserving installs
# ---------------------------------------------------------------------------
# Every file published below goes through `install -C` semantics (--compare),
# generated ones (axl.pc, axl-config.cmake, share/axl metadata) included: if the destination
# already holds identical bytes, it is left ALONE — mtime included.
#
# This is not a micro-optimization, it is the contract consumers depend on. A
# consumer building against a checkout reinstalls the SDK on every build (an
# order-only prerequisite is the usual pattern), and gcc depfiles list SDK
# headers because they arrive via -isystem. With an unconditional `cp` every
# no-op reinstall refreshed ~290 header mtimes, so every no-op consumer build
# recompiled essentially its whole tree — measured downstream at 84 of 85
# objects. Consumers were forced to hand-roll content fingerprints to work
# around it. Pinned by test/integration/test-install-idempotent.sh.
#
# `install -C` and NOT `cp -u`: cp -u compares MTIMES, so a destination that is
# newer than its source but differs in content is silently left in place (a
# hand-edited installed header, a restored-from-backup prefix). Content compare
# is the semantics that actually matches intent. Both GNU and BSD install
# implement -C this way.

# install_c MODE SRC... DEST -- `install -C -m MODE`, with every CHANGED file
# landing by RENAME. A test may be linking from this prefix while another
# install refreshes it (run-integration.sh restages before its pool), and an
# in-place copy lets that reader see half a file; a rename gives it the old
# file or the new one. As with `install -C`, identical bytes leave the
# destination alone, mtime included -- test-install-idempotent.sh pins that --
# and a SYMLINK destination is always replaced, never written through.
# Unlike it, a destination whose bytes match but whose mode differs has its
# mode fixed in place rather than being re-copied (which keeps the mtime).
# The scratch is a dotfile beside the destination, so a run killed between
# the copy and the rename cannot leave something that looks like a command
# in libexec/axl. DEST is a directory when it is one, else the target path.
install_c() {
    local mode="$1" dest="${!#}" src dst base tmp
    shift
    local -a srcs=("${@:1:$#-1}")
    for src in "${srcs[@]}"; do
        if [[ -d "$dest" && ! -L "$dest" ]]; then
            dst="$dest/${src##*/}"
        else
            dst="$dest"
        fi
        if [[ ! -L "$dst" && -f "$dst" ]] && cmp -s "$src" "$dst"; then
            [[ "$(stat -c %a "$dst")" == "$mode" ]] || chmod "$mode" "$dst"
            continue
        fi
        base="${dst##*/}"
        tmp=$(mktemp "${dst%/*}/.${base}.axl-tmp.XXXXXX")
        if ! install -m "$mode" "$src" "$tmp" || ! mv -f "$tmp" "$dst"; then
            rm -f "$tmp"
            log_error "install_c: could not stage $dst"
            return 1
        fi
    done
}

# axl_install_trace EVENT -- append EVENT to $AXL_INSTALL_TRACE when set: the
# order of the build-tree lock's events, which test-install-concurrent.sh
# asserts. A no-op otherwise.
axl_install_trace() {
    [[ -z "${AXL_INSTALL_TRACE:-}" ]] || echo "$*" >> "$AXL_INSTALL_TRACE"
}


# Validate tools
log_info "Backend: native (gcc + ld + objcopy)"
for tool in gcc ld ar objcopy; do
    if ! command -v "$tool" &>/dev/null; then
        log_error "'$tool' not found in PATH"
        exit 1
    fi
done

# C++ toolchain detection.  In "auto" mode the absence of the
# toolchain is silent — pure-C consumers see no diagnostics for a
# feature they didn't ask for.  In "require" mode (--cpp) the same
# absence is a hard error with the install instruction.  "skip"
# (--no-cpp) bypasses detection entirely.
# Toolchain locations come from ONE file shared with the CMake build, axl-cc and
# the axl-config.cmake generated below -- see scripts/axl-toolchains.conf.
TOOLCHAIN_CONF="$SCRIPT_DIR/axl-toolchains.conf"
if [[ ! -r "$TOOLCHAIN_CONF" ]]; then
    log_error "toolchain manifest missing: $TOOLCHAIN_CONF"
    exit 1
fi
# shellcheck source=/dev/null
. "$TOOLCHAIN_CONF"

# AXL_TOOLCHAIN -- the same variant the CMake build and axl-cc take. This is
# the THIRD build path, and it resolves the compiler independently in order to bake
# a real path into the generated axl-config.cmake, so a variant the other two
# honour and this one ignores is exactly the drift `gate.check-flag-parity`
# exists over. The failure was the quiet kind: under `cross` with a locator
# left unset, the ${:-} fallbacks below would stamp the /opt default into a
# consumer's CMake package, and the detection a few lines down would announce
# "C++ toolchain detected" about a toolchain the caller never named.
#
# It also has to happen HERE, before detection: the CMake configure catches an
# unknown variant eventually, but only after this script has logged two
# reassuring lines and started a build.
AXL_TOOLCHAIN="${AXL_TOOLCHAIN:-axl}"
case "$AXL_TOOLCHAIN" in
    axl) ;;
    auto)
        # MAPPED to axl, not rejected. `auto` is axl-cc's x64 default and the
        # value its --verbose reports, so it is the spelling a consumer is
        # most likely to have exported -- and there is nothing here for it to
        # choose between, since the SDK builds ITSELF bare-metal either way
        # (AXL-Host-Toolchain-Design.md section 8). The packager must not be
        # the one build path where the default's own name is a fatal typo.
        AXL_TOOLCHAIN="axl"
        ;;
    host)
        # Refused with its own reason rather than falling into the
        # unknown-variant arm below: `host` IS a variant, just not one that
        # builds this. Same three facts as the CMake build and axl-cc's help --
        # a variant the three build paths describe differently is the drift
        # `gate.check-flag-parity` exists over.
        log_error "AXL_TOOLCHAIN=host does not build the SDK."
        log_error "It governs what a CONSUMER compiles with axl-cc; the SDK"
        log_error "builds ITSELF with the bare-metal toolchain on both arches"
        log_error "(docs/AXL-Host-Toolchain-Design.md section 8)."
        log_error "Use 'axl' here -- or 'auto', which means 'axl' for this"
        log_error "build -- and set AXL_TOOLCHAIN=host on axl-cc when you"
        log_error "compile against the result."
        exit 1
        ;;
    cross)
        AXL_X64_GCC_DEFAULT=""
        AXL_X64_GXX_DEFAULT=""
        AXL_X64_BINUTILS_PREFIX_DEFAULT=""
        AXL_AA64_GCC_DEFAULT=""
        AXL_AA64_GXX_DEFAULT=""
        AXL_AA64_BINUTILS_PREFIX_DEFAULT=""
        ;;
    *)
        log_error "AXL_TOOLCHAIN=$AXL_TOOLCHAIN is not a toolchain variant."
        log_error "Use 'axl' (the default), 'auto' (the same thing here, and"
        log_error "axl-cc's x64 default) or 'cross' (one you supply, named"
        log_error "by AXL_X64_GCC / AXL_AA64_GCC and the matching _GXX and"
        log_error "_BINUTILS_PREFIX)."
        exit 1
        ;;
esac

ARM_GXX="${AXL_AA64_GXX:-$AXL_AA64_GXX_DEFAULT}"
# C++ compiles bare-metal on BOTH arches now (AXL-Cxx-Design.md §6a-PLAN task
# T2). This was the host g++ -- the SDK's last host input -- and dropping it is
# not only a hermeticity argument: the host compiler is glibc-targeted, so its
# libsupc++ reads the exception globals and the stack canary through %fs, which
# UEFI never sets up.
X64_GXX="${AXL_X64_GXX:-$AXL_X64_GXX_DEFAULT}"
# C compiles bare-metal on BOTH arches now, so the generated CMake package has
# to carry a real path rather than ${AXL_CROSS}gcc: host gcc would resolve
# <string.h> to /usr/include, and the aa64 Linux cross has no <string.h> at all.
ARM_GCC="${AXL_AA64_GCC:-$AXL_AA64_GCC_DEFAULT}"
# ld/ar/objcopy for aa64, from ARM's toolchain rather than apt's
# aarch64-linux-gnu-*: a consumer should need no system development tools
# (AXL-Libc-Substrate-Design.md §4.1d).
ARM_BINUTILS_PREFIX="${AXL_AA64_BINUTILS_PREFIX:-$AXL_AA64_BINUTILS_PREFIX_DEFAULT}"
X64_BINUTILS_PREFIX="${AXL_X64_BINUTILS_PREFIX:-$AXL_X64_BINUTILS_PREFIX_DEFAULT}"
X64_GCC="${AXL_X64_GCC:-$AXL_X64_GCC_DEFAULT}"
BUILD_CPP=0
# Resolve architectures. BEFORE the C++ toolchain check below, which now keys
# off BUILD_ARCHS: validating afterwards let `--arch amd64 --cpp` pass both
# toolchain checks vacuously and log "C++ toolchain detected" before dying on
# "unknown arch" -- a reassuring line about a build that never happens.
case "$BUILD_ARCHS" in
    all) ARCHES=(x64 aa64) ;;
    x64) ARCHES=(x64) ;;
    aa64) ARCHES=(aa64) ;;
    *) echo "ERROR: unknown arch '$BUILD_ARCHS'" >&2; exit 1 ;;
esac

# --print-build-lock: report the lock path(s) the build below would take, then
# exit before doing anything. It runs the same `axl-build.sh --print-prefix`
# QUERY the build runs, so a caller cannot end up reasoning about a path this script does
# not take -- which is the whole reason it is reported rather than documented.
if [[ "$PRINT_BUILD_LOCK" == "1" ]]; then
    for arch in "${ARCHES[@]}"; do
        # The SAME query the build uses. An earlier draft of this block spelled
        # the prefix out and appended -tls by hand, and was already wrong by
        # one suffix against the build ten lines down -- which is why this asks
        # rather than composes, and why dropping the suffix touched nothing here.
        _p="$("$LIBAXL_DIR/scripts/axl-build.sh" --print-prefix ARCH="$arch" BUILD=RELEASE)"
        printf '%s\n' "$LIBAXL_DIR/$_p.buildlock"
    done
    exit 0
fi

# Only an arch actually being built imposes a toolchain requirement. Checking
# both unconditionally made `--arch x64 --cpp` hard-fail on a missing AArch64
# bare-metal g++ that the run was never going to invoke -- which is precisely
# what CI's integration job does, so that step had never once succeeded.
wants_arch() {  # <arch>
    local a
    for a in "${ARCHES[@]}"; do
        [[ "$a" == "$1" ]] && return 0
    done
    return 1
}

# Under `cross` every locator is mandatory, and nobody else will say so early.
# The build does refuse (cmake/AxlToolchain.cmake, which always builds C, C++
# and the image pipeline's binutils), but only after this script logs
# "Building (x64, ...)" and hands off -- so the reader sees a build start and
# then a build error, when the fault was an unset variable known before any of
# that.
if [[ "$AXL_TOOLCHAIN" == "cross" ]]; then
    for _a in "${ARCHES[@]}"; do
        case "$_a" in
            x64)  _need=("AXL_X64_GCC=$X64_GCC" "AXL_X64_GXX=$X64_GXX"
                         "AXL_X64_BINUTILS_PREFIX=$X64_BINUTILS_PREFIX") ;;
            aa64) _need=("AXL_AA64_GCC=$ARM_GCC" "AXL_AA64_GXX=$ARM_GXX"
                         "AXL_AA64_BINUTILS_PREFIX=$ARM_BINUTILS_PREFIX") ;;
            *)    continue ;;
        esac
        for _kv in "${_need[@]}"; do
            [[ -n "${_kv#*=}" ]] && continue
            _var="${_kv%%=*}"
            log_error "AXL_TOOLCHAIN=cross needs $_var set to your own"
            log_error "bare-metal toolchain's for $_a. The axl-toolchains.conf"
            log_error "defaults are deliberately not consulted under this"
            log_error "variant -- set it, or unset AXL_TOOLCHAIN to use the"
            log_error "toolchain the SDK installs."
            exit 1
        done
    done
fi

if [[ "$CPP_MODE" != "skip" ]]; then
    cpp_missing=""
    if wants_arch x64 \
       && ! command -v "$X64_GXX" &>/dev/null && [[ ! -x "$X64_GXX" ]]; then
        cpp_missing="$X64_GXX not found (needed for x64 C++ builds)"
    elif wants_arch aa64 && [[ ! -x "$ARM_GXX" ]]; then
        cpp_missing="ARM bare-metal g++ not found at $ARM_GXX"
    fi
    if [[ -z "$cpp_missing" ]]; then
        BUILD_CPP=1
        log_info "C++ toolchain detected — will build the C++ runtime glue"
    elif [[ "$CPP_MODE" == "require" ]]; then
        log_error "$cpp_missing"
        log_error "Run scripts/install-toolchain.sh first, or omit --cpp."
        exit 1
    fi
    # auto-mode + missing toolchain: silently fall through to C-only
fi


# ---------------------------------------------------------------------------
# Build, install, publish.
#
# WHAT THE PREFIX HOLDS IS NOT DECIDED HERE. It is cmake/AxlInstall.cmake's
# install() rules, which CPack's release archives are built from too
# (docs/AXL-Build-System-Design.md §10.16). This script builds each arch's
# RELEASE tree, runs `cmake --install` of it into a private scratch prefix,
# and PUBLISHES the scratch into --prefix with install_c: content-compared and
# by rename, which `cmake --install` straight into --prefix would not be (it
# compares mtimes, so an identical rebuild refreshes every mtime, and it
# writes in place, so a concurrent reader can see half a file).
#
# The layout (relocatable: axl.pc uses ${pcfiledir}, axl-config.cmake and
# axl-cc resolve their prefix from their own location):
#
#   <prefix>/bin/        axl, axl-cc, axl-c++, axl-install-toolchain,
#                        pe-set-debug, axl-efi-pack
#   <prefix>/include/axl-sdk/   axl.h, axl/, uefi/
#   <prefix>/lib/axl/<arch>/    libaxl.a, libaxl-standin.a, runtime objects,
#                               pack/ (the axl-cc --compress stub)
#   <prefix>/lib/axl/           linker scripts, efi-localize.ver
#   <prefix>/lib/cmake/axl/axl-config.cmake, lib/pkgconfig/axl{,-<arch>}.pc
#   <prefix>/libexec/axl/       the host tools `axl <cmd>` runs
#   <prefix>/share/axl/         version, build-date, backend, sidecars,
#                               converters, axl-toolchains.conf
# ---------------------------------------------------------------------------

# This script runs `cmake --install` itself, so it takes the pinned CMake
# like every other entry point (scripts/axl-cmake-pin.sh, §10.13).
# shellcheck source=./axl-cmake-pin.sh
source "$LIBAXL_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

mkdir -p "$PREFIX"
SCRATCH="$(mktemp -d -t axl-install.XXXXXXXX)"
# On INT/TERM too: a bare EXIT trap does not run when a signal kills the
# shell, and what would be left is a whole two-arch SDK in $TMPDIR.
axl_trap_cleanup 'rm -rf "$SCRATCH"'
# A DESTDIR in the environment (packaging environments export one) would send
# `cmake --install` to $DESTDIR$SCRATCH, and the publish below would find the
# scratch empty. This script's destination is --prefix, nothing else.
unset DESTDIR

# The arch-neutral components come from ONE tree: the last built, which also
# supplies the unsuffixed axl.pc ("the last arch built", as it always has).
LAST_ARCH="${ARCHES[${#ARCHES[@]}-1]}"

for arch in "${ARCHES[@]}"; do
    # THE BUILD TREE: scripts/axl-build.sh's RELEASE preset tree, ASKED of
    # it, never composed -- a hardcoded prefix once silently defeated the
    # tree split of the day and made two configurations wipe each other's
    # objects. RELEASE, not the DEBUG tree the tests build, so the two never
    # share objects of different flags.
    local_prefix="$("$LIBAXL_DIR/scripts/axl-build.sh" --print-prefix ARCH="$arch" BUILD=RELEASE)"
    log_info "Building ($arch, gcc, RELEASE)..."
    # SERIALISED on the build tree, not on install.sh: --prefix says where to
    # STAGE, but every run builds the same tree, and three integration tests
    # run install.sh concurrently. Held across the build AND the install out
    # of the tree -- releasing it between the two let a run copy a
    # half-written archive while another rebuilt it. Keyed on the tree, so
    # unrelated configurations still run in parallel.
    _build_lock="$LIBAXL_DIR/$local_prefix.buildlock"
    mkdir -p "$(dirname "$_build_lock")"
    # `sdk`: what the install rules read (cmake/AxlGoals.cmake; CMake's `all`
    # is every image). The build's output goes to a log: its last lines on
    # success, ALL of it on failure -- a configure error is many lines, and
    # its last three name none of the cause.
    _do_build() {
        local _log
        _log="$(mktemp -t axl-install-build.XXXXXXXX)"
        if ! "$LIBAXL_DIR/scripts/axl-build.sh" -j"$(nproc)" \
                ARCH="$arch" BUILD=RELEASE \
                $( [[ "$BUILD_CPP" == "1" ]] && echo "AXL_CPP=1" ) \
                sdk > "$_log" 2>&1; then
            cat "$_log" >&2
            rm -f "$_log"
            log_error "the $arch RELEASE build failed (above)"
            return 1
        fi
        tail -3 "$_log"
        rm -f "$_log"
    }
    if command -v flock >/dev/null 2>&1; then
        # fd 9, held until the install below is done; a failure under `set -e`
        # ends the process, which closes the fd and releases it. No timeout:
        # a bounded wait that gave up would reintroduce the corruption.
        exec 9>"$_build_lock"
        flock 9
        axl_install_trace "lock $arch"
    else
        # A stripped container. Build unserialised rather than refusing, but
        # say so: a silent downgrade of a correctness measure is how the
        # original defect stayed invisible.
        log_warning "flock not found: concurrent install.sh runs may corrupt $local_prefix"
    fi
    _do_build
    axl_install_trace "build $arch"

    # The components (cmake/AxlInstall.cmake). The C++ glue only when C++ is
    # in: a C-only install (--no-cpp) ships no half of it.
    _components=(sdk_arch)
    [[ "$BUILD_CPP" == "1" ]] && _components+=(sdk_cxxrt)
    [[ "$arch" == "$LAST_ARCH" ]] && _components+=(sdk_host sdk_common)
    _ilog="$SCRATCH/install-$arch.log"
    for _c in "${_components[@]}"; do
        if ! cmake --install "$LIBAXL_DIR/$local_prefix" --prefix "$SCRATCH/$arch" \
                --component "$_c" >> "$_ilog" 2>&1; then
            cat "$_ilog" >&2
            log_error "cmake --install of $arch's $_c component failed (above)"
            exit 1
        fi
    done

    # The last read of the built tree: the lock can go.
    axl_install_trace "copied $arch"
    if command -v flock >/dev/null 2>&1; then
        flock -u 9
        exec 9>&-
        axl_install_trace "unlock $arch"
    fi
    log_info "Installed $arch (${_components[*]})"
done

# publish_tree SRC DEST -- every directory and file under SRC into DEST,
# files through install_c with the mode the install rules gave them. The
# listing is taken FIRST and checked: read through a process substitution,
# a find that could not run would publish nothing and return 0, and "found
# nothing" and "could not look" must not read the same. A symlink, or a file
# whose destination is a directory, is refused: no rule makes either, and
# install_c would copy into the directory.
publish_tree() {
    local src="$1" dest="$2" list kind mode rel n=0
    list="$SCRATCH/publish-${src##*/}.list"
    if ! (cd "$src" && find . -mindepth 1 -printf '%y %m %P\n') > "$list.raw" \
            || ! LC_ALL=C sort -k3 "$list.raw" > "$list"; then
        log_error "publish: could not list $src"
        return 1
    fi
    while IFS=' ' read -r kind mode rel; do
        case "$kind" in
            d) mkdir -p "$dest/$rel" ;;
            f) if [[ -d "$dest/$rel" && ! -L "$dest/$rel" ]]; then
                   log_error "publish: $dest/$rel is a directory, not a file"
                   return 1
               fi
               install_c "$mode" "$src/$rel" "$dest/$rel"
               n=$((n + 1)) ;;
            *) log_error "publish: $src/$rel is neither a file nor a directory"
               return 1 ;;
        esac
    done < "$list"
    if (( n == 0 )); then
        log_error "publish: cmake --install put no file in $src"
        return 1
    fi
}
# SERIALISED on the prefix: each file lands atomically, but two runs
# publishing at once -- or one publishing an older build after another
# published a newer -- could leave a prefix of mixed builds. The lock is the
# prefix DIRECTORY itself, so nothing is written beside or inside it.
if command -v flock >/dev/null 2>&1; then
    exec 7<"$PREFIX"
    flock 7
fi
for arch in "${ARCHES[@]}"; do
    publish_tree "$SCRATCH/$arch" "$PREFIX"
done
log_info "Installed axl-config.cmake, axl.pc, axl-cc, axl-c++, axl-install-toolchain"
log_info "Installed axl dispatcher + host tools"

# Point out a staged SDK left at either HISTORICAL default -- ./out or the
# source root itself. Neither is stale the day it is written; both go stale
# silently. The candidates live in axl-common.sh, the one list.
axl_warn_stale_sdk_prefix "$PREFIX" "$LIBAXL_DIR"

# Remove a compat/ left behind by an OLDER install into this same prefix: an
# in-place upgrade would keep serving its stale shims, whose `typedef void
# FILE` shadowed the toolchain's real <stdio.h> (AXL-Libc-Substrate-Design.md
# §4.1b). install.sh stopped creating it; that is not the same as removing it.
rm -rf "$PREFIX/include/axl-sdk/compat"

# ---------------------------------------------------------------------------
# `current` symlink (AXL-Distribution-Design.md §12.2)
#
# axl-prune.sh has always PROTECTED whatever <root>/axl-sdk resolves to, and
# nothing has ever created it -- so that protection was unreachable unless a
# user made the link by hand, and consumers had no stable path to pin.
#
# GUARDED to a versioned root. install.sh's default prefix is ./stage in a
# checkout, and a bare `install.sh` must not start dropping symlinks into the
# parent of the source tree. Only a prefix that is itself `axl-sdk-<version>`
# gets one, which is exactly the layout the SDK tarball extracts to.
# ---------------------------------------------------------------------------
_prefix_base="$(basename "$PREFIX")"
if [[ "$_prefix_base" =~ ^axl-sdk-[0-9] ]]; then
    _root="$(dirname "$PREFIX")"
    # -n so an existing symlink is REPLACED rather than followed into the
    # directory it points at, which would nest axl-sdk/axl-sdk-<ver>.
    if ln -sfn "$_prefix_base" "$_root/axl-sdk" 2>/dev/null; then
        log_info "current -> $_root/axl-sdk -> $_prefix_base"
    else
        log_warning "could not update $_root/axl-sdk (not writable?)"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

log_info "AXL SDK installed to $PREFIX (toolchain: gcc)"
echo ""
echo "  Build app:    $PREFIX/bin/axl-cc hello.c -o hello.efi"
echo "  Build driver: $PREFIX/bin/axl-cc --type driver mydrv.c -o mydrv.efi"
echo "  CMake:        find_package(axl REQUIRED)"
echo "                axl_add_app(hello hello.c)"
