#!/bin/bash
# Build AXL and package the SDK for standalone use.
#
# Usage: ./scripts/install.sh [OPTIONS]
#
# Options:
#   --prefix DIR           Install location (default: ./out)
#   --arch ARCH            Build only this arch: x64, aa64, or all (default: all)
#
# Requires: gcc, ld, ar, objcopy (GCC toolchain)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

# Defaults
PREFIX="$SDK_DIR/out"
LIBAXL_DIR="$SDK_DIR"
BUILD_ARCHS="all"
# C++ support mode: "auto" (default — build if toolchain present, skip
# silently otherwise), "require" (--cpp: fail loud if missing), "skip"
# (--no-cpp: don't build even if present).
CPP_MODE="auto"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)      PREFIX="$2"; shift 2 ;;
        --arch)        BUILD_ARCHS="$2"; shift 2 ;;
        --cpp)         CPP_MODE="require"; shift ;;
        --no-cpp)      CPP_MODE="skip"; shift ;;
        -h|--help)
            cat <<HELP
Usage: $0 [--prefix DIR] [--arch x64|aa64|all] [--cpp|--no-cpp]

C++ support is built automatically when the C++ toolchain is
present (aarch64-none-elf-g++ at /opt for AArch64, host g++ for
x64). Otherwise the install is C-only — no warning, no error.

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

ARM_GXX="${AXL_AA64_GXX:-$AXL_AA64_GXX_DEFAULT}"
X64_GXX="${AXL_X64_GXX:-g++}"
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
        log_info "C++ toolchain detected — will build libaxl-cxx.a"
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
         "$PREFIX/include/axl-sdk/compat" \
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
    local_prefix="out/native-$arch-release"
    log_info "Building ($arch, gcc, RELEASE)..."
    make -C "$LIBAXL_DIR" \
        ARCH="$arch" PREFIX="$local_prefix" BUILD=RELEASE \
        ${AXL_TLS:+AXL_TLS=$AXL_TLS} \
        $( [[ "$BUILD_CPP" == "1" ]] && echo "AXL_CPP=1" ) \
        -j "$(nproc)" 2>&1 | tail -3

    mkdir -p "$PREFIX/lib/axl/$arch"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/lib/libaxl.a"              "$PREFIX/lib/axl/$arch/"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-crt0-native.o"   "$PREFIX/lib/axl/$arch/"
    install -C -m 644 "$LIBAXL_DIR/$local_prefix/build/axl-crt0-minimal.o"  "$PREFIX/lib/axl/$arch/"
    if [[ "$BUILD_CPP" == "1" ]]; then
        install -C -m 644 "$LIBAXL_DIR/$local_prefix/lib/libaxl-cxx.a"      "$PREFIX/lib/axl/$arch/"
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
Cflags: -I${includedir} -I${includedir}/compat
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
# Hosted-libc shims (<string.h>, <stdlib.h>, ...) — the SAME headers the
# library builds against (Makefile -Iinclude/compat). Consumers that include
# them directly (a ported app) must get the SDK's freestanding shims, not host
# glibc: the aa64 cross-gcc has no glibc at all (build fails without these),
# and x64 must not silently borrow /usr/include. axl-cc adds this dir to
# -isystem so `#include <string.h>` resolves here on every arch.
install -C -m 644 "$LIBAXL_DIR/include/compat/"*.h                "$PREFIX/include/axl-sdk/compat/"

# GCC linker scripts live next to the per-arch lib data.
install -C -m 644 "$LIBAXL_DIR/scripts/elf_x86_64_efi.lds"  "$PREFIX/lib/axl/"
install -C -m 644 "$LIBAXL_DIR/scripts/elf_aarch64_efi.lds" "$PREFIX/lib/axl/"
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

sed -e "s|@AXL_AA64_GXX@|$_arm_gxx_esc|g" \
    -e "s|@AXL_X64_GXX@|$_x64_gxx_esc|g" \
    << 'CMAKE_SUPPORT' | write_if_changed "$PREFIX/lib/cmake/axl/axl-config.cmake"
# axl-config.cmake — CMake support for building AXL applications (GCC toolchain).
#
# Usage (preferred):
#   find_package(axl REQUIRED)
#   axl_add_app(myapp myapp.c)                  # → myapp.efi
#   axl_add_app(myapp myapp.cpp)                # C++ (freestanding)
#   axl_add_app(myapp myapp.cpp HOSTED)         # C++ with std::vector/string/map
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
    set(AXL_CROSS "")
    # CMake list (unquoted) so ${AXL_GCC_ARCH} expands to multiple
    # args in COMMAND — a quoted string would pass as one arg and
    # gcc would reject "-mno-red-zone -march=x86-64" wholesale.
    set(AXL_GCC_ARCH -mno-red-zone -march=x86-64)
    set(AXL_PE_TARGET "pei-x86-64")
    set(AXL_EFI_LDS "${AXL_SDK_DIR}/lib/axl/elf_x86_64_efi.lds")
    set(AXL_GCC_CRT0 "${AXL_LIB_DIR}/axl-crt0-gcc-x86_64.o")
elseif(AXL_ARCH STREQUAL "aa64")
    set(AXL_CROSS "aarch64-linux-gnu-")
    # -ffixed-x18: UEFI AArch64 binding (UEFI 2.11 §2) reserves x18
    # as the platform register; without this gcc may clobber it,
    # leading to post-ExitBootServices OS-side corruption.
    set(AXL_GCC_ARCH -ffixed-x18)
    set(AXL_PE_TARGET "pei-aarch64-little")
    set(AXL_EFI_LDS "${AXL_SDK_DIR}/lib/axl/elf_aarch64_efi.lds")
    set(AXL_GCC_CRT0 "${AXL_LIB_DIR}/axl-crt0-gcc-aarch64.o")
endif()

set(AXL_CRT0_NATIVE "${AXL_LIB_DIR}/axl-crt0-native.o")
set(AXL_RELOC_OBJ   "${AXL_LIB_DIR}/axl-reloc.o")
set(AXL_DEBUG_OBJ   "${AXL_LIB_DIR}/axl-debug-info.o")
set(AXL_PE_SET_DEBUG "${AXL_SDK_DIR}/bin/pe-set-debug")

# Stack-smashing detection, matching the Makefile and axl-cc.
#
# -mstack-protector-guard=global is the load-bearing half on x86-64: GCC
# otherwise reads the canary from %fs:0x28, glibc's TLS block, which UEFI never
# sets up -- so the plain flag faults instead of protecting. AArch64 already
# defaults to the global symbol. libaxl.a supplies __stack_chk_guard and
# __stack_chk_fail.
#
# This is a THIRD copy of the compile flags (Makefile, axl-cc, here) and it
# drifted: it kept -fno-stack-protector after the other two turned the
# protector on, so a CMake-built app was unprotected while an axl-cc-built one
# was not -- same SDK, same source, different security posture depending on
# which build path the consumer picked. `make check-flag-parity` asserts they
# agree.
set(AXL_STACK_PROTECTOR -fstack-protector-strong -mstack-protector-guard=global)

set(AXL_C_FLAGS
    -ffreestanding -fshort-wchar -fno-builtin
    ${AXL_STACK_PROTECTOR} -fno-omit-frame-pointer -fpic
    -ffunction-sections -fdata-sections
    -Os -Wall
    -DAXL_BACKEND_NATIVE
)

# C++ flag set, mirroring axl-cc's: no exceptions, no RTTI, no thread-safe
# statics, C++23.
set(AXL_CXX_FLAGS
    -ffreestanding -fshort-wchar -fno-builtin
    ${AXL_STACK_PROTECTOR} -fno-omit-frame-pointer -fpic
    -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -std=c++23
    -ffunction-sections -fdata-sections
    -Os -Wall
    -DAXL_BACKEND_NATIVE
)

# HOSTED differs by two REMOVALS: -ffreestanding (libstdc++ refuses the
# containers under it, at bits/requires_hosted.h) and the compat shim include
# path (its `typedef void FILE` collides with the real <stdio.h> that hosted
# libstdc++ pulls in). The arch flags STAY -- dropping them is how a hosted
# spike picked up AVX from a -march=x86-64-v3 default and #UD'd under firmware.
set(AXL_CXX_FLAGS_HOSTED ${AXL_CXX_FLAGS})
list(REMOVE_ITEM AXL_CXX_FLAGS_HOSTED -ffreestanding)

if(AXL_ARCH STREQUAL "aa64")
    # The Linux-ABI cross's libstdc++ headers pull hosted typedefs; the
    # bare-metal "none-elf" toolchain is the one that works. Matches axl-cc.
    # Substituted at install time from scripts/axl-toolchains.conf, so this
    # file cannot drift from the Makefile and axl-cc.
    set(AXL_CXX_COMPILER "@AXL_AA64_GXX@")
else()
    set(AXL_CXX_COMPILER "@AXL_X64_GXX@")
endif()

# Internal helper: build a TARGET.efi from C sources, optional embedded
# blobs, and a chosen image type (app / driver). Most consumers reach
# this via axl_add_app or axl_add_driver — call _axl_build_efi directly
# only if you need behavior neither wrapper exposes.
function(_axl_build_efi TARGET TYPE)
    cmake_parse_arguments(_AXL "HOSTED;ALLOW_UEFI" "" "SOURCES;EMBEDS" ${ARGN})

    # Type → subsystem code + CRT0 wiring.
    #   app:    asm CRT0 → _AxlEntry (C CRT0 axl-crt0-native.o) → main
    #   driver: asm CRT0 → _AxlEntry (aliased to user's entry via --defsym=DriverEntry)
    # Raw UEFI access follows the IMAGE TYPE, matching axl-cc: a driver
    # implements the protocol types it produces or interposes on, an app does
    # not get them by default. ALLOW_UEFI is the deliberate opt-out, and it is
    # visible in the CMakeLists rather than buried in an #include.
    if(_AXL_ALLOW_UEFI OR TYPE STREQUAL "driver")
        set(_UEFI_DEFINE -DAXL_ALLOW_UEFI)
    else()
        set(_UEFI_DEFINE "")
    endif()

    if(TYPE STREQUAL "app")
        set(_SUBSYSTEM 10)
        set(_CRT0_OBJS "${AXL_GCC_CRT0}" "${AXL_RELOC_OBJ}"
                       "${AXL_DEBUG_OBJ}" "${AXL_CRT0_NATIVE}")
        set(_LD_DEFSYM "")
    elseif(TYPE STREQUAL "driver")
        set(_SUBSYSTEM 11)
        set(_CRT0_OBJS "${AXL_GCC_CRT0}" "${AXL_RELOC_OBJ}" "${AXL_DEBUG_OBJ}")
        set(_LD_DEFSYM "--defsym=_AxlEntry=DriverEntry")
    else()
        message(FATAL_ERROR "_axl_build_efi: unknown type '${TYPE}' (expected 'app' or 'driver')")
    endif()

    # Compile C sources to ELF .o. Use TARGET-prefixed object names so
    # the same source file can be compiled into multiple targets without
    # output collisions.
    set(_ALL_OBJS ${_CRT0_OBJS})
    set(_HAS_CPP FALSE)
    foreach(SRC ${_AXL_SOURCES})
        get_filename_component(SRC_NAME ${SRC} NAME_WE)
        get_filename_component(SRC_ABS ${SRC} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        set(OBJ "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.${SRC_NAME}.o")

        # Dispatch per extension, same rule as axl-cc: .cpp/.cc/.cxx -> g++.
        # C sources stay freestanding even under HOSTED -- libaxl.a is
        # freestanding and a mixed image links, exactly as axl-cc does it.
        if(SRC MATCHES "[.](cpp|cc|cxx)$")
            set(_HAS_CPP TRUE)
            set(_CC "${AXL_CXX_COMPILER}")
            if(_AXL_HOSTED)
                # HOSTED drops the compat shims too: their `typedef void FILE`
                # collides with the real <stdio.h> hosted libstdc++ pulls in.
                set(_CC_FLAGS ${AXL_CXX_FLAGS_HOSTED})
                set(_CC_INCS -isystem ${AXL_INCLUDE_DIR})
            else()
                set(_CC_FLAGS ${AXL_CXX_FLAGS})
                set(_CC_INCS -isystem ${AXL_INCLUDE_DIR}
                             -isystem ${AXL_INCLUDE_DIR}/compat)
            endif()
            set(_CC_LABEL "g++")
        else()
            set(_CC "${AXL_CROSS}gcc")
            set(_CC_FLAGS ${AXL_C_FLAGS})
            set(_CC_INCS -isystem ${AXL_INCLUDE_DIR}
                         -isystem ${AXL_INCLUDE_DIR}/compat)
            set(_CC_LABEL "gcc")
        endif()

        add_custom_command(
            OUTPUT ${OBJ}
            COMMAND ${_CC} ${_CC_FLAGS} ${_UEFI_DEFINE} ${AXL_GCC_ARCH} ${_CC_INCS}
                    -c ${SRC_ABS} -o ${OBJ}
            DEPENDS ${SRC_ABS}
            COMMENT "${_CC_LABEL}: ${TARGET} ← ${SRC}"
        )
        list(APPEND _ALL_OBJS ${OBJ})
    endforeach()

    # EMBEDS: each entry is `PATH` or `PATH=name`. For each, emit a tiny
    # .s sidecar containing .incbin, then assemble it and add to the
    # link. The symbol convention (axl_embedded_<name>{,_end}) matches
    # what <axl/axl-embed.h>'s AXL_EMBED_DECLARE expects — same as
    # `axl-cc --embed`.
    foreach(SPEC ${_AXL_EMBEDS})
        if(SPEC MATCHES "^(.*)=([^=]+)$")
            set(_EMBED_PATH "${CMAKE_MATCH_1}")
            set(_EMBED_SYM  "${CMAKE_MATCH_2}")
        else()
            set(_EMBED_PATH "${SPEC}")
            get_filename_component(_EMBED_BASE "${_EMBED_PATH}" NAME_WE)
            string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" _EMBED_SYM "${_EMBED_BASE}")
        endif()
        if(NOT IS_ABSOLUTE "${_EMBED_PATH}")
            set(_EMBED_PATH "${CMAKE_CURRENT_BINARY_DIR}/${_EMBED_PATH}")
        endif()
        if(NOT _EMBED_SYM MATCHES "^[a-zA-Z_][a-zA-Z0-9_]*$")
            message(FATAL_ERROR
                "axl: embed symbol '${_EMBED_SYM}' is not a valid C identifier — "
                "pass EMBEDS path=name explicitly")
        endif()

        set(_EMBED_S "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.embed_${_EMBED_SYM}.s")
        set(_EMBED_O "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.embed_${_EMBED_SYM}.o")

        # Generated at CMake configure time — content depends only on
        # the path string, not on any build-time inputs.
        file(WRITE "${_EMBED_S}"
"    .section .rodata
    .balign 8
    .globl axl_embedded_${_EMBED_SYM}
    .globl axl_embedded_${_EMBED_SYM}_end
axl_embedded_${_EMBED_SYM}:
    .incbin \"${_EMBED_PATH}\"
axl_embedded_${_EMBED_SYM}_end:

    .section .note.GNU-stack, \"\", %progbits
")

        add_custom_command(
            OUTPUT ${_EMBED_O}
            COMMAND ${AXL_CROSS}gcc -c ${_EMBED_S} -o ${_EMBED_O}
            DEPENDS ${_EMBED_S} ${_EMBED_PATH}
            COMMENT "embed: ${TARGET} ← ${_EMBED_PATH} (axl_embedded_${_EMBED_SYM})"
        )
        list(APPEND _ALL_OBJS ${_EMBED_O})
    endforeach()

    set(_SO_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.so")
    set(_EFI_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.efi")

    # C++ needs libaxl-cxx.a and nothing else: operator new/delete, the
    # std::__throw_* entry points, ceil, _Prime_rehash_policy, the
    # _Rb_tree_* helpers, _Hash_bytes and _M_replace_cold all live there, so
    # HOSTED no longer pulls the toolchain's libstdc++.a either. Two archives
    # with an acyclic edge (libaxl-cxx.a references axl_malloc, libaxl.a
    # supplies it) need no --start-group. Same arrangement as axl-cc.
    set(_CXX_LIBS "")
    set(_GROUP_END "")
    if(_HAS_CPP)
        if(NOT EXISTS "${AXL_LIB_DIR}/libaxl-cxx.a")
            message(FATAL_ERROR
                "axl: C++ source needs ${AXL_LIB_DIR}/libaxl-cxx.a, which this "
                "SDK was built without. Rebuild with: install.sh --cpp")
        endif()
        # No libstdc++.a, hosted or not. libaxl-cxx.a supplies everything the
        # containers need out of line (the _Rb_tree_* helpers, _Hash_bytes,
        # the std::__throw_* entry points, _Prime_rehash_policy,
        # _M_replace_cold), so the archive is off the line entirely -- which is
        # what makes the SDK self-contained AND keeps it clear of the one act
        # the GCC Runtime Library Exception does not cover.
        #
        # This block used to mirror axl-cc's old behaviour and was missed when
        # axl-cc changed. That is the drift `make check-flag-parity` exists
        # for, except it compares FLAGS and objcopy -j names, not archives --
        # so it could not see this.
        #
        # -frtti is the exception (typeid / dynamic_cast need libsupc++'s
        # type_info vtables). A CMake consumer wanting it should add
        # libstdc++.a to their own target's link libraries; axl_add_app does
        # not detect per-source flags.
        set(_CXX_LIBS ${AXL_LIB_DIR}/libaxl-cxx.a)
    endif()

    add_custom_command(
        OUTPUT ${_SO_FILE}
        COMMAND ${AXL_CROSS}ld -nostdlib -shared -Bsymbolic
                --no-warn-rwx-segments --no-undefined
                ${_LD_DEFSYM}
                -T ${AXL_EFI_LDS}
                -o ${_SO_FILE}
                ${_ALL_OBJS}
                ${_CXX_LIBS}
                ${AXL_LIB_DIR}/libaxl.a
                ${_GROUP_END}
        DEPENDS ${_ALL_OBJS}
        COMMENT "ld: ${TARGET}.so"
    )

    add_custom_command(
        OUTPUT ${_EFI_FILE}
        COMMAND ${AXL_CROSS}objcopy
                -j .text -j .sdata -j .data -j .bss -j .dynamic -j .dynsym
                -j .rel -j .rela -j .rela.dyn -j .reloc -j .rodata -j .dbgdir
                --output-target=${AXL_PE_TARGET} --subsystem=${_SUBSYSTEM}
                ${_SO_FILE} ${_EFI_FILE}
        COMMAND ${AXL_PE_SET_DEBUG} ${_EFI_FILE}
        DEPENDS ${_SO_FILE}
        COMMENT "objcopy+pe-set-debug: ${TARGET}.efi"
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
    cmake_parse_arguments(_AXL_ADD "HOSTED;ALLOW_UEFI" "" "EMBEDS" ${ARGN})
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
    cmake_parse_arguments(_AXL_DRV "HOSTED;ALLOW_UEFI" "" "" ${ARGN})
    if(_AXL_DRV_HOSTED)
        set(_HOSTED_ARG HOSTED)
    else()
        set(_HOSTED_ARG "")
    endif()
    _axl_build_efi(${TARGET} "driver" ${_HOSTED_ARG}
        SOURCES ${_AXL_DRV_UNPARSED_ARGUMENTS})
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
# axl-c++ ships UNCONDITIONALLY, unlike libaxl-cxx.a. It is a 9-line `exec
# axl-cc -x c++` wrapper with no build step and no dependency on the C++
# toolchain, so BUILD_CPP would not be saving anything — and gating it made the
# failure WORSE. axl-cc already diagnoses both C++ failure modes precisely at
# the moment of use ("g++ not found ... run axl-install-toolchain aa64",
# "libaxl-cxx.a is missing ... run ./scripts/install.sh --cpp"). Withholding
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
