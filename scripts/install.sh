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
# Every copy below goes through `install -C` (--compare): if the destination
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

# Write stdin to $1, but only if that changes the file. The generated-content
# equivalent of `install -C` — a regenerated .pc / .cmake / metadata file whose
# bytes are unchanged must not bump its mtime either.
write_if_changed() {
    local dst="$1" tmp rc=0
    # A directory destination would make `install` silently write INTO it under
    # the scratch file's random name and report success — the plain `>` this
    # replaced failed loudly ("Is a directory"). Keep failing loudly.
    if [[ -d "$dst" ]]; then
        log_error "write_if_changed: destination is a directory: $dst"
        return 1
    fi
    tmp=$(mktemp "${dst}.XXXXXX")
    # EVERY exit path below removes the scratch explicitly. errexit would
    # otherwise abort mid-function (a full disk or quota during `cat`) and strand
    # a random-suffixed file in the install tree that no later run ever matches
    # — straight into whatever a packaging step scoops up.
    if ! cat > "$tmp"; then
        rm -f "$tmp"
        log_error "write_if_changed: failed writing $dst"
        return 1
    fi
    install -C -m 644 "$tmp" "$dst" || rc=$?
    rm -f "$tmp"
    return "$rc"
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
# Toolchain locations come from ONE file shared with the Makefile, axl-cc and
# the axl-config.cmake generated below -- see scripts/axl-toolchains.conf.
TOOLCHAIN_CONF="$SCRIPT_DIR/axl-toolchains.conf"
if [[ ! -r "$TOOLCHAIN_CONF" ]]; then
    log_error "toolchain manifest missing: $TOOLCHAIN_CONF"
    exit 1
fi
# shellcheck source=/dev/null
. "$TOOLCHAIN_CONF"

# AXL_TOOLCHAIN -- the same variant the Makefile and axl-cc take. This is the
# THIRD build path, and it resolves the compiler independently in order to bake
# a real path into the generated axl-config.cmake, so a variant the other two
# honour and this one ignores is exactly the drift `make check-flag-parity`
# exists over. The failure was the quiet kind: under `cross` with a locator
# left unset, the ${:-} fallbacks below would stamp the /opt default into a
# consumer's CMake package, and the detection a few lines down would announce
# "C++ toolchain detected" about a toolchain the caller never named.
#
# It also has to happen HERE, before detection: make catches an unknown variant
# eventually, but only after this script has logged two reassuring lines and
# started a build.
AXL_TOOLCHAIN="${AXL_TOOLCHAIN:-axl}"
case "$AXL_TOOLCHAIN" in
    axl) ;;
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
        log_error "Use 'axl' (the default) or 'cross' (one you supply, named"
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
# exit before doing anything. It runs the same `make print-prefix` QUERY the
# build runs, so a caller cannot end up reasoning about a path this script does
# not take -- which is the whole reason it is reported rather than documented.
if [[ "$PRINT_BUILD_LOCK" == "1" ]]; then
    for arch in "${ARCHES[@]}"; do
        # The SAME query the build uses. An earlier draft of this block spelled
        # the prefix out and appended -tls by hand, and was already wrong by
        # one suffix against the build ten lines down -- which is why this asks
        # rather than composes, and why dropping the suffix touched nothing here.
        _p="$(make -s -C "$LIBAXL_DIR" ARCH="$arch" BUILD=RELEASE \
            print-prefix)"
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

# Under `cross` the C compiler is mandatory and nobody else will say so early.
# make does refuse, but only after this script logs "Building (x64, ...)" and
# hands off -- so the reader sees a build start and then a Makefile error, when
# the actual fault was an unset variable known before any of that. C++ stays
# optional here exactly as it is under `axl`; this is only about C, which every
# build needs.
if [[ "$AXL_TOOLCHAIN" == "cross" ]]; then
    for _a in "${ARCHES[@]}"; do
        case "$_a" in
            x64)  _cc="$X64_GCC"; _var="AXL_X64_GCC" ;;
            aa64) _cc="$ARM_GCC"; _var="AXL_AA64_GCC" ;;
            *)    continue ;;
        esac
        if [[ -z "$_cc" ]]; then
            log_error "AXL_TOOLCHAIN=cross needs $_var set to your own"
            log_error "bare-metal C compiler for $_a. The axl-toolchains.conf"
            log_error "defaults are deliberately not consulted under this"
            log_error "variant -- set it, or unset AXL_TOOLCHAIN to use the"
            log_error "toolchain the SDK installs."
            exit 1
        fi
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

# Build and package per-arch.
#
# FHS-polished layout (files the package manager owns):
#
#   <prefix>/bin/axl-cc, pe-set-debug
#   <prefix>/include/axl.h, axl/, uefi/
#   <prefix>/lib/axl/<arch>/libaxl.a, axl-crt0-*.o, axl-reloc.o, ...
#   <prefix>/lib/axl/elf_<arch>_efi.lds
#   <prefix>/lib/cmake/axl/axl-config.cmake       (find_package(axl))
#   <prefix>/lib/pkgconfig/axl{,-<arch>}.pc        (pkg-config)
#   <prefix>/share/axl/{version,build-date,backend}      (metadata)
#   <prefix>/share/axl/{pci-ids,pci-class,usb-ids,jedec}.json5
#                                                        (optional sidecars)
#   <prefix>/share/axl/scripts/{pci,usb}-ids-to-json5.py + _ids_parser.py
#                                                        (bulk converters)
#
# Everything is relocatable — the cmake, pkg-config, and axl-cc files
# resolve paths relative to their own installed location, so a .deb
# dropping these under /usr works identically to a tarball unpacked
# under /opt/axl-sdk.

# Headers are namespaced under include/axl-sdk/ rather than living
# directly under include/. This keeps /usr/include itself out of the
# axl-cc command line on system installs (PREFIX=/usr) — otherwise
# `-isystem /usr/include` shadows cross-gcc's freestanding stdint.h
# with host glibc's, which then chains to gnu/stubs-32.h and breaks
# aa64 cross-builds on hosts (RHEL/Alma) that don't ship 32-bit stubs.
# axl-cc / cmake / pkg-config all point at <prefix>/include/axl-sdk
# so user `#include <axl.h>` and `#include <axl/axl-mem.h>` resolve
# unchanged.
mkdir -p "$PREFIX/bin" \
         "$PREFIX/include/axl-sdk/axl" \
         "$PREFIX/include/axl-sdk/uefi/generated" \
         "$PREFIX/lib/axl" "$PREFIX/lib/cmake/axl" "$PREFIX/lib/pkgconfig" \
         "$PREFIX/share/axl"

AXL_VERSION=$(cat "$SDK_DIR/VERSION")

for arch in "${ARCHES[@]}"; do
    # Build into a BUILD-mode-segregated prefix so install.sh's
    # RELEASE artifacts don't poison the in-tree DEBUG cache that
    # `make ARCH=$arch tests` (default BUILD=DEBUG) reuses. Sharing
    # the prefix used to silently produce a mixed-flag libaxl.a:
    # the .o cache key includes only the .c timestamp, not the
    # BUILD mode, so a RELEASE-built axl-mem.o would survive into
    # a subsequent DEBUG build and the alloc-fill / fence machinery
    # would be missing — visible as the test_debug_features 0xDA
    # flake.
    # ASK for the prefix, never compose it. Passing PREFIX on make's command
    # line OVERRIDES the Makefile's own rule, and this line used to hardcode
    # "out/native-$arch-release" -- which silently defeated the tree split of
    # the day and made two configurations wipe each other's objects (~300 at a
    # time), or, concurrently, corrupt them: "the input file ... is empty" and
    # a stale staged prefix both came from here. The AXL_TLS half of that split
    # is gone, but the rule that prevented it is why removing it was safe.
    local_prefix="$(make -s -C "$LIBAXL_DIR" ARCH="$arch" BUILD=RELEASE \
        print-prefix)"
    log_info "Building ($arch, gcc, RELEASE)..."
    # SERIALISED on the build tree, not on install.sh. --prefix says where to
    # STAGE; the build always lands in local_prefix, so two install.sh runs
    # with different prefixes still aim `make -j` at one target set. Under the
    # integration suite that is routine rather than exotic: three tests invoke
    # install.sh, so they collide on out/native-<arch>-release. Observed
    # symptoms were a partial archive and objcopy's "the input file ... is
    # empty"; one such loss left a test linking against a three-week-old staged
    # prefix and failing on a missing symbol, which reads as a library defect.
    #
    # This guards two builds of the SAME configuration. There is only one
    # configuration now, which makes the collision MORE likely, not less.
    #
    # The lock is keyed on the tree so unrelated configurations still run in
    # parallel, and it is held only across the build -- staging into distinct
    # prefixes is genuinely concurrent and stays that way.
    _build_lock="$LIBAXL_DIR/$local_prefix.buildlock"
    mkdir -p "$(dirname "$_build_lock")"
    _do_build() {
        make -C "$LIBAXL_DIR" \
            ARCH="$arch" PREFIX="$local_prefix" BUILD=RELEASE \
            $( [[ "$BUILD_CPP" == "1" ]] && echo "AXL_CPP=1" ) \
            -j "$(nproc)" 2>&1 | tail -3
    }
    if command -v flock >/dev/null 2>&1; then
        # fd 9, and the subshell scopes it so the lock is released even if the
        # build fails under `set -e`. No timeout: waiting is the correct
        # behaviour, and a bounded wait that gave up would reintroduce exactly
        # the corruption this prevents.
        ( flock 9 || exit 1; _do_build ) 9>"$_build_lock"
    else
        # No flock (a stripped container). Build unserialised rather than
        # refusing -- a single install.sh is the common case and is unaffected
        # -- but say so, because a silent downgrade of a correctness measure is
        # how the original defect stayed invisible.
        log_warning "flock not found: concurrent install.sh runs may corrupt $local_prefix"
        _do_build
    fi

    mkdir -p "$PREFIX/lib/axl/$arch"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/lib/libaxl.a"              "$PREFIX/lib/axl/$arch/"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-crt0-native.o"   "$PREFIX/lib/axl/$arch/"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-crt0-minimal.o"  "$PREFIX/lib/axl/$arch/"
    if [[ "$BUILD_CPP" == "1" ]]; then
        # THE C++ RUNTIME GLUE, staged as OBJECTS rather than as
        # libaxl-cxxrt.a, and the ONLY C++-specific thing the SDK ships now:
        # P4 deleted libaxl-cxx.a and put the toolchain's real libstdc++ on
        # every C++ link instead (AXL-Libc-Substrate-Design.md §4d).
        #
        # axl-cxxrt-alloc.o supplies the sbrk newlib's dlmalloc grows into,
        # and axl-cxxrt-terminate.o preempts libstdc++'s verbose terminate
        # handler (~112 KB of demangler and stdio that prints nothing under
        # UEFI). Neither works from an archive: the member being displaced
        # gets pulled for some other symbol and either multiply-defines or
        # arrives with its dependencies intact. axl-cc names all four
        # individually on every C++ link.
        # axl-cxxrt-nothrow.o is the --no-eh-frame alternative to
        # axl-cxxrt-eh.o: the two are mutually exclusive on a link, and
        # axl-cc picks one. Both are staged so the choice is the consumer's.
        for _ehobj in axl-cxxrt-alloc.o axl-cxxrt-eh.o axl-cxxrt-nothrow.o \
                      axl-cxxrt-stubs.o axl-cxxrt-terminate.o; do
            if [[ -f "$LIBAXL_DIR/$local_prefix/build/$_ehobj" ]]; then
                install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/$_ehobj" \
                    "$PREFIX/lib/axl/$arch/"
            fi
        done
    fi

    # GCC needs: assembly CRT0, reloc object, linker script
    if [[ "$arch" == "aa64" ]]; then
        install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-crt0-gcc-aarch64.o" "$PREFIX/lib/axl/$arch/"
    else
        install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-crt0-gcc-x86_64.o"  "$PREFIX/lib/axl/$arch/"
    fi
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-reloc.o"      "$PREFIX/lib/axl/$arch/"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-debug-info.o" "$PREFIX/lib/axl/$arch/"
    install -C -m 755 "$LIBAXL_DIR/$local_prefix/build/pe-set-debug"     "$PREFIX/bin/"

    # Per-arch pkg-config file. Uses pkg-config's built-in ${pcfiledir}
    # so the resulting file is relocatable — the prefix is computed
    # relative to the .pc file's own location.
    pc_arch="$PREFIX/lib/pkgconfig/axl-$arch.pc"
    sed -e "s|@AXL_ARCH@|$arch|g" -e "s|@AXL_VERSION@|$AXL_VERSION|g" <<'PCEOF' | write_if_changed "$pc_arch"
prefix=${pcfiledir}/../..
exec_prefix=${prefix}
libdir=${prefix}/lib/axl/@AXL_ARCH@
includedir=${prefix}/include/axl-sdk

Name: axl
Description: AximCode Library - GLib-inspired C library for UEFI (@AXL_ARCH@)
URL: https://axl.aximcode.com
Version: @AXL_VERSION@
Libs: -L${libdir} -laxl
Cflags: -I${includedir}
PCEOF

    # Plain `axl.pc` so `pkg-config axl` works without an arch suffix.
    # In multi-arch installs this reflects the last arch built; single
    # arch installs get exactly that arch.
    install -C -m 644 "$pc_arch" "$PREFIX/lib/pkgconfig/axl.pc"

    log_info "Installed libraries for $arch"
done
log_info "Installed axl.pc (pkg-config)"

# Headers (including uefi/generated/ subdirectory).
# Staged under include/axl-sdk/ so the axl-cc -isystem flag points
# at a SDK-only directory, not /usr/include directly. See note above.
install -C -m 644 "$LIBAXL_DIR/include/axl.h"                     "$PREFIX/include/axl-sdk/"
install -C -m 644 "$LIBAXL_DIR/include/axl/"*.h                   "$PREFIX/include/axl-sdk/axl/"
# The C++ layer. Shipped unconditionally, not gated on AXL_CPP: these are
# header-only and guarded by `#ifndef __cplusplus -> #error`, so a pure-C
# consumer that never includes one pays nothing, and a C++ consumer of a
# C-only build gets a clear diagnostic instead of a missing file.
install -C -m 644 "$LIBAXL_DIR/include/axl/"*.hpp                 "$PREFIX/include/axl-sdk/axl/"
install -C -m 644 "$LIBAXL_DIR/include/uefi/"*.h                  "$PREFIX/include/axl-sdk/uefi/"
install -C -m 644 "$LIBAXL_DIR/include/uefi/generated/"*.h        "$PREFIX/include/axl-sdk/uefi/generated/"
# Point out a staged SDK left at either HISTORICAL default -- ./out (where
# --prefix pointed before O1 moved it to ./stage) or the source root itself
# (`install.sh --prefix .`, older still). Neither is stale the day it is
# written; both go stale silently, and the source-root one is the case that
# makes scripts/axl-cc answer for a build weeks behind its own checkout.
#
# The candidates and the removal advice live in axl-common.sh, not here: two
# spellings of "where a stale SDK can hide" is precisely how the source root
# went uncovered for the whole life of the ./out warning.
axl_warn_stale_sdk_prefix "$PREFIX" "$LIBAXL_DIR"

# Remove a compat/ left behind by an OLDER install into this same prefix.
# install.sh stopped CREATING it, which is not the same as removing it: an
# in-place upgrade would keep serving the stale shims -- and their
# `typedef void FILE` would shadow the toolchain's real <stdio.h> for every
# consumer, silently, on a tree that looks correctly installed.
rm -rf "$PREFIX/include/axl-sdk/compat"

# NOTE: the hosted-libc shims that used to be staged here are gone. They
# existed because a consumer including <string.h> had nowhere else to get one —
# the aa64 cross-gcc ships no glibc at all, and x64 would silently borrow
# /usr/include. Both are answered properly now: axl-cc compiles C with the
# bare-metal cross on both arches, whose newlib supplies the genuine headers,
# and the SDK uses no host headers or libraries.
# docs/AXL-Libc-Substrate-Design.md §4.1b.

# GCC linker scripts live next to the per-arch lib data.
install -C -m 644 "$LIBAXL_DIR/scripts/elf_x86_64_efi.lds"  "$PREFIX/lib/axl/"
install -C -m 644 "$LIBAXL_DIR/scripts/elf_aarch64_efi.lds" "$PREFIX/lib/axl/"
# The EXCEPTIONS variants, selected by axl-cc when -fexceptions is in play.
# Separate files because the KEEP(*(.eh_frame)) they carry costs a C-only image
# +16.8% for tables it can never use (AXL-Cxx-Unwinder-Design.md §U2).
install -C -m 644 "$LIBAXL_DIR/scripts/elf_x86_64_efi_eh.lds"  "$PREFIX/lib/axl/"
install -C -m 644 "$LIBAXL_DIR/scripts/elf_aarch64_efi_eh.lds" "$PREFIX/lib/axl/"
# Linker version script: localizes symbols so --gc-sections shrinks the .efi.
install -C -m 644 "$LIBAXL_DIR/scripts/efi-localize.ver"    "$PREFIX/lib/axl/"

# Metadata — lives under share/ since it's arch-independent plain text
# that axl-cc reads for --version output and sanity checks.
echo "native"          | write_if_changed "$PREFIX/share/axl/backend"
cat "$SDK_DIR/VERSION" | write_if_changed "$PREFIX/share/axl/version"
# build-date is genuinely time-varying, so it updates when the UTC day rolls
# over and is stable within a day — write_if_changed keeps it that way rather
# than bumping it on every install.
date -u '+%Y-%m-%d'    | write_if_changed "$PREFIX/share/axl/build-date"

# Optional JSON5 sidecars consumed by axl_*_ids_load. Shipped as
# reference content so consumers don't have to clone the SDK to get
# a starter database — they typically copy these next to their .efi
# (the auto-discovery companion path) or pass --ids-file explicitly.
# New class triplets, USB vendors, JEDEC manufacturer codes can all
# land via a JSON5 update without rebuilding any consumer binary.
for sidecar in pci-ids.json5 \
               usb-ids.json5 jedec.json5; do
    if [[ -f "$LIBAXL_DIR/share/$sidecar" ]]; then
        install -C -m 644 "$LIBAXL_DIR/share/$sidecar" "$PREFIX/share/axl/$sidecar"
    fi
done

# Bulk-conversion scripts for the canonical pci.ids / usb.ids text
# databases. Installed under share/axl/scripts/ so consumers who
# need the full tail (vs the curated starter sets above) can run
# e.g. `python3 /usr/share/axl/scripts/usb-ids-to-json5.py
# /usr/share/hwdata/usb.ids > usb-ids.json5` to generate fleet-
# scale OEM-rebadge coverage. JEDEC has no canonical text database
# so no jedec-to-json5.py — manual curation, see share/jedec.json5.
mkdir -p "$PREFIX/share/axl/scripts"
for converter in _ids_parser.py pci-ids-to-json5.py usb-ids-to-json5.py; do
    if [[ -f "$LIBAXL_DIR/scripts/$converter" ]]; then
        install -C -m 755 "$LIBAXL_DIR/scripts/$converter" \
                          "$PREFIX/share/axl/scripts/$converter"
    fi
done


# ---------------------------------------------------------------------------
# Generate axl-config.cmake (enables `find_package(axl)`)
# ---------------------------------------------------------------------------

# The heredoc is QUOTED so CMake's own ${...} survives verbatim; the toolchain
# path is injected through a sed placeholder instead, the same way the
# pkg-config file above handles @AXL_ARCH@. That keeps this third build path
# reading from scripts/axl-toolchains.conf rather than carrying its own copy.
# Escape the sed REPLACEMENT metacharacters before injecting. Unescaped, `&`
# expands to the whole match (a path containing one is silently corrupted into
# ".../@AXL_AA64_GXX@..."), `|` ends the s-command and hard-errors mid-install,
# and `\` is swallowed. Only reachable through an override with an exotic path,
# but silent corruption of a shipped CMake package is not worth the risk.
sed_escape_repl() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }
_arm_gxx_esc="$(sed_escape_repl "$ARM_GXX")"
_x64_gxx_esc="$(sed_escape_repl "$X64_GXX")"
_arm_gcc_esc="$(sed_escape_repl "$ARM_GCC")"
_x64_gcc_esc="$(sed_escape_repl "$X64_GCC")"
_arm_binutils_esc="$(sed_escape_repl "$ARM_BINUTILS_PREFIX")"
_x64_binutils_esc="$(sed_escape_repl "$X64_BINUTILS_PREFIX")"

sed -e "s|@AXL_AA64_GXX@|$_arm_gxx_esc|g" \
    -e "s|@AXL_X64_GXX@|$_x64_gxx_esc|g" \
    -e "s|@AXL_AA64_GCC@|$_arm_gcc_esc|g" \
    -e "s|@AXL_X64_GCC@|$_x64_gcc_esc|g" \
    -e "s|@AXL_AA64_BINUTILS_PREFIX@|$_arm_binutils_esc|g" \
    -e "s|@AXL_X64_BINUTILS_PREFIX@|$_x64_binutils_esc|g" \
    << 'CMAKE_SUPPORT' | write_if_changed "$PREFIX/lib/cmake/axl/axl-config.cmake"
# axl-config.cmake — CMake support for building AXL applications (GCC toolchain).
#
# Usage (preferred):
#   find_package(axl REQUIRED)
#   axl_add_app(myapp myapp.c)                  # → myapp.efi
#   axl_add_app(myapp myapp.cpp)                # C++, std::vector/string/map included
#                                               # (HOSTED was removed; it is an error)
#   axl_add_driver(myDxe myDxe.c)               # → myDxe.efi (DriverEntry, subsystem 11)
#   axl_add_app(myapp myapp.c ALLOW_UEFI)       # app that needs <uefi/...>
#                                               # (drivers get it from TYPE)
#   axl_add_app(my-launcher launcher.c          # launcher embeds the driver:
#       EMBEDS ${CMAKE_CURRENT_BINARY_DIR}/myDxe.efi=my_driver
#   )                                           # → my-launcher.efi (with .incbin'd blob)
#
# Also works via explicit include:
#   include(/usr/lib/cmake/axl/axl-config.cmake)
#   axl_add_app(myapp myapp.c)

# Resolve the SDK root from this file's installed location:
#   <prefix>/lib/cmake/axl/axl-config.cmake  →  <prefix>
if(NOT DEFINED AXL_SDK_DIR)
    get_filename_component(AXL_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()

if(NOT DEFINED AXL_ARCH)
    set(AXL_ARCH "x64")
endif()

set(AXL_INCLUDE_DIR "${AXL_SDK_DIR}/include/axl-sdk")
set(AXL_LIB_DIR     "${AXL_SDK_DIR}/lib/axl/${AXL_ARCH}")

if(AXL_ARCH STREQUAL "x64")
    # Our own binutils, so a consumer needs no system development tools.
    # CMake list (unquoted) so ${AXL_GCC_ARCH} expands to multiple
    # args in COMMAND — a quoted string would pass as one arg and
    # gcc would reject "-mno-red-zone -march=x86-64" wholesale.
elseif(AXL_ARCH STREQUAL "aa64")
    # @AXL_AA64_BINUTILS_PREFIX@, not apt's aarch64-linux-gnu-: a consumer
    # should need no system development tools, and ARM's toolchain ships the
    # pei-aarch64-little target this package converts to. x64 keeps the empty
    # prefix (host binutils) until its toolchain is rebuilt with PE support --
    # see AXL-Libc-Substrate-Design.md §4.1d.
    # -ffixed-x18: UEFI AArch64 binding (UEFI 2.11 §2) reserves x18
    # as the platform register; without this gcc may clobber it,
    # leading to post-ExitBootServices OS-side corruption.
endif()

# The driver every image is built by. This package used to reimplement what it
# does; it calls it now, so there is one place the flags live.
set(AXL_CC "${AXL_SDK_DIR}/bin/axl-cc")
if(NOT EXISTS "${AXL_CC}")
    message(FATAL_ERROR
        "axl: ${AXL_CC} is missing -- this SDK install is incomplete.")
endif()

# Stack-smashing detection, matching the Makefile and axl-cc.
#
# -mstack-protector-guard=global is the load-bearing half on x86-64: GCC
# otherwise reads the canary from %fs:0x28, glibc's TLS block, which UEFI never
# sets up -- so the plain flag faults instead of protecting. AArch64 already
# defaults to the global symbol. libaxl.a supplies __stack_chk_guard and
# __stack_chk_fail.
#
# THE COMPILE FLAGS ARE NOT HERE ANY MORE, and that is the point.
#
# This used to be a THIRD copy of them (Makefile, axl-cc, here), and it
# drifted: it kept -fno-stack-protector after the other two turned the
# protector on, so a CMake-built app was unprotected while an axl-cc-built one
# was not -- same SDK, same source, different security posture depending on
# which build path the consumer picked. It drifted again later, missing
# -fexceptions support entirely.
#
# _axl_build_efi calls axl-cc now, so the flags live in one place and this file
# cannot disagree with it. `make check-flag-parity` still polices the remaining
# two paths and asserts THIS one delegates -- reintroduce a hand-rolled compile
# here and it rejoins the comparison.

if(AXL_ARCH STREQUAL "aa64")
    # The Linux-ABI cross's libstdc++ headers pull hosted typedefs; the
    # bare-metal "none-elf" toolchain is the one that works. Matches axl-cc.
    # Substituted at install time from scripts/axl-toolchains.conf, so this
    # file cannot drift from the Makefile and axl-cc.
    set(AXL_CXX_COMPILER "@AXL_AA64_GXX@")
    set(AXL_C_COMPILER   "@AXL_AA64_GCC@")
else()
    set(AXL_CXX_COMPILER "@AXL_X64_GXX@")
    set(AXL_C_COMPILER   "@AXL_X64_GCC@")
endif()

# Internal helper: build a TARGET.efi from C sources, optional embedded
# blobs, and a chosen image type (app / driver). Most consumers reach
# this via axl_add_app or axl_add_driver — call _axl_build_efi directly
# only if you need behavior neither wrapper exposes.
function(_axl_build_efi TARGET TYPE)
    cmake_parse_arguments(_AXL "HOSTED;ALLOW_UEFI" "" "SOURCES;EMBEDS;OPTIONS" ${ARGN})

    # THIS FUNCTION CALLS axl-cc. It used to re-implement it -- its own compile
    # line per source, its own ld, its own objcopy, its own pe-set-debug, ~200
    # lines mirroring scripts/axl-cc.
    #
    # That duplication was the THIRD of the three build paths
    # `make check-flag-parity` exists to police, and it was not merely untidy:
    # when axl-c++ gained -fexceptions, this copy did not, so a consumer asking
    # for exceptions here got an image that compiled, linked, and died at the
    # first throw -- and the parity gate could not see it, because the gate
    # compares flag SPELLINGS and both paths named the same objcopy sections.
    # It also had no way to pass a compile flag at all: axl_add_app produces a
    # custom target, so target_compile_options() errors with "non-compilable
    # target type", which is how the missing -fexceptions was found.
    #
    # Calling the driver fixes all of that by construction. Anything axl-cc
    # learns, CMake consumers get; there is one place for the flags to live.
    # The cost is one process per image instead of one per source, which is
    # nothing next to the compile itself, and losing per-source parallelism --
    # acceptable because axl-cc is what a consumer would type by hand anyway.
    if(_AXL_HOSTED)
        message(FATAL_ERROR
            "axl: HOSTED was removed -- C++ is compiled hosted unconditionally, "
            "so the keyword selects nothing. Delete it from ${TARGET}; the "
            "output is unchanged. (docs/AXL-Cxx-Design.md 6a-T3)")
    endif()

    if(NOT _AXL_SOURCES)
        message(FATAL_ERROR "axl: ${TARGET} has no SOURCES")
    endif()

    # Raw UEFI access follows the IMAGE TYPE, matching axl-cc: a driver
    # implements the protocol types it produces or interposes on, an app does
    # not get them by default. ALLOW_UEFI is the deliberate opt-out, visible in
    # the CMakeLists rather than buried in an #include. axl-cc applies the
    # type rule itself, so only the explicit opt-out has to be forwarded.
    set(_AXL_CC_ARGS "")
    if(_AXL_ALLOW_UEFI)
        list(APPEND _AXL_CC_ARGS --allow-uefi)
    endif()

    # EMBEDS: `PATH` or `PATH=name`, forwarded verbatim -- axl-cc takes the
    # same spelling and owns the .incbin sidecar, the symbol-name derivation
    # and the validation that a derived name is a C identifier.
    #
    # A relative path is resolved against the BINARY dir, not the source dir,
    # because the overwhelmingly common embed is a driver .efi that another
    # axl_add_driver() just produced. That was this function's behaviour before
    # and is preserved deliberately.
    set(_EMBED_DEPS "")
    foreach(SPEC ${_AXL_EMBEDS})
        if(SPEC MATCHES "^(.+)=([^=]+)$")
            set(_EMBED_PATH "${CMAKE_MATCH_1}")
            set(_EMBED_NAME "=${CMAKE_MATCH_2}")
        else()
            set(_EMBED_PATH "${SPEC}")
            set(_EMBED_NAME "")
        endif()
        if(NOT IS_ABSOLUTE "${_EMBED_PATH}")
            set(_EMBED_PATH "${CMAKE_CURRENT_BINARY_DIR}/${_EMBED_PATH}")
        endif()
        list(APPEND _AXL_CC_ARGS --embed "${_EMBED_PATH}${_EMBED_NAME}")
        list(APPEND _EMBED_DEPS "${_EMBED_PATH}")
    endforeach()

    # Sources as ABSOLUTE paths: axl-cc runs in the build dir, and CMake
    # resolves a relative SOURCES entry against the source dir.
    set(_SRC_ABS "")
    foreach(SRC ${_AXL_SOURCES})
        get_filename_component(_A "${SRC}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND _SRC_ABS "${_A}")
    endforeach()

    set(_EFI_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.efi")

    # TWO STEPS -- compile each source, then link -- rather than one axl-cc
    # invocation over all of them. Two reasons, and the second is the one that
    # forces it:
    #
    #   - per-source objects keep `make -jN` parallelism, which a single
    #     compile-and-link call would throw away;
    #   - dependency files are COMPILE-ONLY (they describe one TU), so a
    #     one-shot call cannot produce one, and header dependency tracking is
    #     the thing this package most needs. The previous re-implementation had
    #     NONE -- its DEPENDS listed only the source -- so editing an SDK or
    #     project header rebuilt nothing.
    #
    # The LINK step needs no COMPILE flags repeated to it. axl-cc derives what
    # the link needs (libstdc++/libsupc++, the C++ linker script and the
    # cxxrt glue objects) from the OBJECTS via `nm -u`,
    # precisely so a staged build works -- so re-passing -O2 or -D would be
    # noise, and forgetting to would be a silent defect. This is that design
    # paying off.
    #
    # OPTIONS is forwarded to the link anyway, and the reason is at that call
    # site: it is the only flag channel this package offers, so it legitimately
    # carries link-side entries (-Wl,-Map=..., --minimal-runtime). Compile-only
    # entries are inert there. An earlier version of this paragraph said the
    # link "deliberately gets no OPTIONS", which described the code before that
    # fix and shipped to consumers inside axl-config.cmake.
    # PLAIN gcc -MD, not an axl-cc-specific flag. There was a `--depfile`
    # option that post-processed the .d to make every path absolute, invented
    # because a RELATIVE source made gcc emit compile-cwd-relative
    # prerequisites that CMake resolved against the wrong directory. This
    # function passes ABSOLUTE sources, so gcc's own output is already absolute
    # and the workaround has nothing left to do.
    #
    # -MD and NOT -MMD, which is what --depfile used internally and is why this
    # is not merely a rename: -MMD omits SYSTEM headers, the SDK arrives via
    # -isystem, so editing an SDK header did NOT rebuild a consumer's object.
    # Caught by the tracking assertions in test-cmake-package.sh.
    set(_DEPFILE_ARGS "")
    if(POLICY CMP0116)
        cmake_policy(SET CMP0116 NEW)
    endif()

    set(_OBJS "")
    set(_I 0)
    foreach(SRC ${_SRC_ABS})
        get_filename_component(_BASE "${SRC}" NAME_WE)
        set(_OBJ "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.${_I}.${_BASE}.o")
        set(_DEP "${_OBJ}.d")
        set(_DEPFILE_ARGS "")
        set(_CC_DEPFILE_ARGS "")
        if(POLICY CMP0116)
            set(_DEPFILE_ARGS DEPFILE "${_DEP}")
            set(_CC_DEPFILE_ARGS -MD -MP -MF "${_DEP}")
        endif()
        add_custom_command(
            OUTPUT ${_OBJ}
            COMMAND ${AXL_CC}
                    --arch ${AXL_ARCH}
                    --type ${TYPE}
                    ${_AXL_CC_ARGS}
                    ${_AXL_OPTIONS}
                    ${_CC_DEPFILE_ARGS}
                    -c ${SRC}
                    -o ${_OBJ}
            DEPENDS ${SRC}
            ${_DEPFILE_ARGS}
            COMMENT "axl-cc: ${TARGET}.${_BASE}.o"
            VERBATIM
        )
        list(APPEND _OBJS "${_OBJ}")
        math(EXPR _I "${_I} + 1")
    endforeach()

    # OPTIONS reaches the LINK as well as the compile. It is the only flag
    # channel this package offers, and a consumer's OPTIONS legitimately holds
    # link-side ones -- `-Wl,-Map=...`, `-Xlinker`, `--entry`,
    # `--minimal-runtime`. Compile-only entries (`-O2`, `-D...`) are inert
    # here, which is the cheap direction to be wrong in; dropping the link-side
    # ones was silent, which is not.
    add_custom_command(
        OUTPUT ${_EFI_FILE}
        COMMAND ${AXL_CC}
                --arch ${AXL_ARCH}
                --type ${TYPE}
                ${_AXL_CC_ARGS}
                ${_AXL_OPTIONS}
                ${_OBJS}
                -o ${_EFI_FILE}
        DEPENDS ${_OBJS} ${_EMBED_DEPS}
        COMMENT "axl-cc: ${TARGET}.efi"
        VERBATIM
    )

    add_custom_target(${TARGET} ALL DEPENDS ${_EFI_FILE})
endfunction()

# Public: build an application image (subsystem APPLICATION, main()).
#
#   axl_add_app(myapp myapp.c)
#   axl_add_app(my-launcher launcher.c
#       EMBEDS ${CMAKE_CURRENT_BINARY_DIR}/myDxe.efi=my_driver
#   )
#
# EMBEDS entries each take the form `path` or `path=name`. Each blob is
# wrapped in a .incbin sidecar and linked into the app; consumer code
# reaches it via <axl/axl-embed.h>'s AXL_EMBED_DECLARE / DATA / SIZE.
function(axl_add_app TARGET)
    # OPTIONS: extra flags forwarded verbatim to axl-cc, e.g.
    #   axl_add_app(app app.cpp OPTIONS -fexceptions -O2)
    # target_compile_options() CANNOT be used on these targets -- axl_add_app
    # produces a custom target, so CMake rejects it with "non-compilable target
    # type". Until OPTIONS existed there was no way to pass a compile flag
    # through this package AT ALL, which is how -fexceptions was unreachable
    # here long after axl-c++ supported it.
    cmake_parse_arguments(_AXL_ADD "HOSTED;ALLOW_UEFI" "" "EMBEDS;OPTIONS" ${ARGN})
    # Forwarded explicitly: cmake_parse_arguments STRIPS a recognised option
    # from UNPARSED_ARGUMENTS, so without this the flag silently does nothing.
    if(_AXL_ADD_HOSTED)
        set(_HOSTED_ARG HOSTED)
    else()
        set(_HOSTED_ARG "")
    endif()
    if(_AXL_ADD_ALLOW_UEFI)
        set(_UEFI_ARG ALLOW_UEFI)
    else()
        set(_UEFI_ARG "")
    endif()
    _axl_build_efi(${TARGET} "app" ${_HOSTED_ARG} ${_UEFI_ARG}
        SOURCES ${_AXL_ADD_UNPARSED_ARGUMENTS}
        EMBEDS  ${_AXL_ADD_EMBEDS}
        OPTIONS ${_AXL_ADD_OPTIONS}
    )
    # Re-export the .efi path so a launcher can EMBED it without
    # re-deriving the path.
    set(${TARGET}_EFI_PATH "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.efi"
        PARENT_SCOPE)
endfunction()

# Public: build a DXE-style driver image (subsystem BOOT_SERVICE_DRIVER,
# DriverEntry). Used for the "shared driver" pattern where a launcher
# loads + LocateProtocol's into a resident driver image.
#
#   axl_add_driver(my-dxe my-dxe.c)
#   axl_add_app(my-launcher launcher.c
#       EMBEDS ${my-dxe_EFI_PATH}=my_driver
#   )
#   add_dependencies(my-launcher my-dxe)
function(axl_add_driver TARGET)
    # ALLOW_UEFI is accepted and ignored: a driver already has raw UEFI from its
    # TYPE. Unregistered, cmake_parse_arguments would leave it in
    # UNPARSED_ARGUMENTS, where it becomes a SOURCE named "ALLOW_UEFI" and the
    # build fails with "no such file" instead of "you do not need it".
    cmake_parse_arguments(_AXL_DRV "HOSTED;ALLOW_UEFI" "" "OPTIONS" ${ARGN})
    if(_AXL_DRV_HOSTED)
        set(_HOSTED_ARG HOSTED)
    else()
        set(_HOSTED_ARG "")
    endif()
    _axl_build_efi(${TARGET} "driver" ${_HOSTED_ARG}
        SOURCES ${_AXL_DRV_UNPARSED_ARGUMENTS}
        OPTIONS ${_AXL_DRV_OPTIONS})
    set(${TARGET}_EFI_PATH "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.efi"
        PARENT_SCOPE)
endfunction()
CMAKE_SUPPORT

log_info "Installed axl-config.cmake"

# ---------------------------------------------------------------------------
# Generate axl-cc
# ---------------------------------------------------------------------------

# Install axl-cc — standalone bash script lives at scripts/axl-cc.
# Until 2026-05-28 this was a ~390-line heredoc embedded here, which
# made the script hard to review, hard to iterate on (edit-then-
# install round-trip), and invisible to shellcheck. Extracting it
# cost nothing and gained all those.
install -C -m 755 "$LIBAXL_DIR/scripts/axl-cc" "$PREFIX/bin/axl-cc"
# axl-c++ ships UNCONDITIONALLY, unlike the C++ glue objects. It is a 9-line `exec
# axl-cc -x c++` wrapper with no build step and no dependency on the C++
# toolchain, so BUILD_CPP would not be saving anything — and gating it made the
# failure WORSE. axl-cc already diagnoses both C++ failure modes precisely at
# the moment of use ("g++ not found ... run axl-install-toolchain aa64",
# "axl-cxxrt-alloc.o ... run ./scripts/install.sh --cpp"). Withholding
# the wrapper replaces those with the shell's bare "axl-c++: command not
# found", which names no remedy. It was also incoherent: `axl-cc hello.cpp`
# reached the good diagnostic on a C-only install while `axl-c++ hello.cpp`
# did not, for identical intent.
install -C -m 755 "$LIBAXL_DIR/scripts/axl-c++" "$PREFIX/bin/axl-c++"

# The toolchain manifest and its installer must SHIP, not just exist in the
# source tree. axl-cc reads the manifest to locate the C++ cross compiler, and
# its "toolchain missing" diagnostic names the installer as the remedy -- but
# only bin/, include/, lib/ and share/ are packaged, so a .deb/.rpm user has no
# scripts/ directory. The old message pointed at scripts/install-arm-toolchain.sh
# and was therefore a dead end for exactly the users who needed it most.
install -C -m 644 "$LIBAXL_DIR/scripts/axl-toolchains.conf" \
                  "$PREFIX/share/axl/axl-toolchains.conf"
install -C -m 755 "$LIBAXL_DIR/scripts/install-toolchain.sh" \
                  "$PREFIX/bin/axl-install-toolchain"
log_info "Installed axl-cc + axl-c++ + axl-install-toolchain"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

log_info "AXL SDK installed to $PREFIX (toolchain: gcc)"
echo ""
echo "  Build app:    $PREFIX/bin/axl-cc hello.c -o hello.efi"
echo "  Build driver: $PREFIX/bin/axl-cc --type driver mydrv.c -o mydrv.efi"
echo "  CMake:        find_package(axl REQUIRED)"
echo "                axl_add_app(hello hello.c)"
