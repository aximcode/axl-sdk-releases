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

To acquire the AArch64 C++ toolchain (96 MB tarball under /opt):
  ./scripts/install-arm-toolchain.sh
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
ARM_GXX="/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-g++"
BUILD_CPP=0
if [[ "$CPP_MODE" != "skip" ]]; then
    cpp_missing=""
    if ! command -v g++ &>/dev/null; then
        cpp_missing="g++ not found in PATH (needed for x64 C++ builds)"
    elif [[ ! -x "$ARM_GXX" ]]; then
        cpp_missing="ARM bare-metal g++ not found at $ARM_GXX"
    fi
    if [[ -z "$cpp_missing" ]]; then
        BUILD_CPP=1
        log_info "C++ toolchain detected — will build libaxl-cxx.a + install axl-c++"
    elif [[ "$CPP_MODE" == "require" ]]; then
        log_error "$cpp_missing"
        log_error "Run scripts/install-arm-toolchain.sh first, or omit --cpp."
        exit 1
    fi
    # auto-mode + missing toolchain: silently fall through to C-only
fi

# Resolve architectures
case "$BUILD_ARCHS" in
    all) ARCHES=(x64 aa64) ;;
    x64) ARCHES=(x64) ;;
    aa64) ARCHES=(aa64) ;;
    *) echo "ERROR: unknown arch '$BUILD_ARCHS'" >&2; exit 1 ;;
esac

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
    local_prefix="out/native-$arch-release"
    log_info "Building ($arch, gcc, RELEASE)..."
    make -C "$LIBAXL_DIR" \
        ARCH="$arch" PREFIX="$local_prefix" BUILD=RELEASE \
        ${AXL_TLS:+AXL_TLS=$AXL_TLS} \
        $( [[ "$BUILD_CPP" == "1" ]] && echo "AXL_CPP=1" ) \
        -j "$(nproc)" 2>&1 | tail -3

    mkdir -p "$PREFIX/lib/axl/$arch"
    cp "$LIBAXL_DIR/$local_prefix/lib/libaxl.a"              "$PREFIX/lib/axl/$arch/"
    cp "$LIBAXL_DIR/$local_prefix/build/axl-crt0-native.o"   "$PREFIX/lib/axl/$arch/"
    cp "$LIBAXL_DIR/$local_prefix/build/axl-crt0-minimal.o"  "$PREFIX/lib/axl/$arch/"
    if [[ "$BUILD_CPP" == "1" ]]; then
        cp "$LIBAXL_DIR/$local_prefix/lib/libaxl-cxx.a"      "$PREFIX/lib/axl/$arch/"
    fi

    # GCC needs: assembly CRT0, reloc object, linker script
    if [[ "$arch" == "aa64" ]]; then
        cp "$LIBAXL_DIR/$local_prefix/build/axl-crt0-gcc-aarch64.o" "$PREFIX/lib/axl/$arch/"
    else
        cp "$LIBAXL_DIR/$local_prefix/build/axl-crt0-gcc-x86_64.o"  "$PREFIX/lib/axl/$arch/"
    fi
    cp "$LIBAXL_DIR/$local_prefix/build/axl-reloc.o"      "$PREFIX/lib/axl/$arch/"
    cp "$LIBAXL_DIR/$local_prefix/build/axl-debug-info.o" "$PREFIX/lib/axl/$arch/"
    cp "$LIBAXL_DIR/$local_prefix/build/pe-set-debug"     "$PREFIX/bin/"

    # Per-arch pkg-config file. Uses pkg-config's built-in ${pcfiledir}
    # so the resulting file is relocatable — the prefix is computed
    # relative to the .pc file's own location.
    pc_arch="$PREFIX/lib/pkgconfig/axl-$arch.pc"
    sed -e "s|@AXL_ARCH@|$arch|g" -e "s|@AXL_VERSION@|$AXL_VERSION|g" > "$pc_arch" <<'PCEOF'
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
    cp "$pc_arch" "$PREFIX/lib/pkgconfig/axl.pc"

    log_info "Installed libraries for $arch"
done
log_info "Installed axl.pc (pkg-config)"

# Headers (including uefi/generated/ subdirectory).
# Staged under include/axl-sdk/ so the axl-cc -isystem flag points
# at a SDK-only directory, not /usr/include directly. See note above.
cp "$LIBAXL_DIR/include/axl.h"                     "$PREFIX/include/axl-sdk/"
cp "$LIBAXL_DIR/include/axl/"*.h                   "$PREFIX/include/axl-sdk/axl/"
cp "$LIBAXL_DIR/include/uefi/"*.h                  "$PREFIX/include/axl-sdk/uefi/"
cp "$LIBAXL_DIR/include/uefi/generated/"*.h        "$PREFIX/include/axl-sdk/uefi/generated/"

# GCC linker scripts live next to the per-arch lib data.
cp "$LIBAXL_DIR/scripts/elf_x86_64_efi.lds"  "$PREFIX/lib/axl/"
cp "$LIBAXL_DIR/scripts/elf_aarch64_efi.lds" "$PREFIX/lib/axl/"

# Metadata — lives under share/ since it's arch-independent plain text
# that axl-cc reads for --version output and sanity checks.
echo "native"                    > "$PREFIX/share/axl/backend"
cat "$SDK_DIR/VERSION"           > "$PREFIX/share/axl/version"
date -u '+%Y-%m-%d'              > "$PREFIX/share/axl/build-date"

# Optional JSON5 sidecars consumed by axl_*_ids_load. Shipped as
# reference content so consumers don't have to clone the SDK to get
# a starter database — they typically copy these next to their .efi
# (the auto-discovery companion path) or pass --ids-file explicitly.
# New class triplets, USB vendors, JEDEC manufacturer codes can all
# land via a JSON5 update without rebuilding any consumer binary.
for sidecar in pci-ids.json5 \
               usb-ids.json5 jedec.json5; do
    if [[ -f "$LIBAXL_DIR/share/$sidecar" ]]; then
        cp "$LIBAXL_DIR/share/$sidecar" "$PREFIX/share/axl/$sidecar"
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
        cp "$LIBAXL_DIR/scripts/$converter" \
           "$PREFIX/share/axl/scripts/$converter"
        chmod +x "$PREFIX/share/axl/scripts/$converter"
    fi
done


# ---------------------------------------------------------------------------
# Generate axl-config.cmake (enables `find_package(axl)`)
# ---------------------------------------------------------------------------

cat > "$PREFIX/lib/cmake/axl/axl-config.cmake" << 'CMAKE_SUPPORT'
# axl-config.cmake — CMake support for building AXL applications (GCC toolchain).
#
# Usage (preferred):
#   find_package(axl REQUIRED)
#   axl_add_app(myapp myapp.c)                  # → myapp.efi
#   axl_add_driver(myDxe myDxe.c)               # → myDxe.efi (DriverEntry, subsystem 11)
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

set(AXL_C_FLAGS
    -ffreestanding -fshort-wchar -fno-builtin
    -fno-stack-protector -fno-omit-frame-pointer -fpic
    -ffunction-sections -fdata-sections
    -Os -Wall
    -DAXL_BACKEND_NATIVE
)

# Internal helper: build a TARGET.efi from C sources, optional embedded
# blobs, and a chosen image type (app / driver). Most consumers reach
# this via axl_add_app or axl_add_driver — call _axl_build_efi directly
# only if you need behavior neither wrapper exposes.
function(_axl_build_efi TARGET TYPE)
    cmake_parse_arguments(_AXL "" "" "SOURCES;EMBEDS" ${ARGN})

    # Type → subsystem code + CRT0 wiring.
    #   app:    asm CRT0 → _AxlEntry (C CRT0 axl-crt0-native.o) → main
    #   driver: asm CRT0 → _AxlEntry (aliased to user's entry via --defsym=DriverEntry)
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
    foreach(SRC ${_AXL_SOURCES})
        get_filename_component(SRC_NAME ${SRC} NAME_WE)
        get_filename_component(SRC_ABS ${SRC} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        set(OBJ "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.${SRC_NAME}.o")
        add_custom_command(
            OUTPUT ${OBJ}
            COMMAND ${AXL_CROSS}gcc ${AXL_C_FLAGS} ${AXL_GCC_ARCH}
                    -isystem ${AXL_INCLUDE_DIR}
                    -c ${SRC_ABS} -o ${OBJ}
            DEPENDS ${SRC_ABS}
            COMMENT "gcc: ${TARGET} ← ${SRC}"
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

    add_custom_command(
        OUTPUT ${_SO_FILE}
        COMMAND ${AXL_CROSS}ld -nostdlib -shared -Bsymbolic
                --no-warn-rwx-segments --no-undefined
                ${_LD_DEFSYM}
                -T ${AXL_EFI_LDS}
                -o ${_SO_FILE}
                ${_ALL_OBJS}
                ${AXL_LIB_DIR}/libaxl.a
        DEPENDS ${_ALL_OBJS}
        COMMENT "ld: ${TARGET}.so"
    )

    add_custom_command(
        OUTPUT ${_EFI_FILE}
        COMMAND ${AXL_CROSS}objcopy
                -j .text -j .sdata -j .data -j .bss -j .dynamic -j .dynsym
                -j .rel -j .rela -j .reloc -j .rodata -j .dbgdir
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
    cmake_parse_arguments(_AXL_ADD "" "" "EMBEDS" ${ARGN})
    _axl_build_efi(${TARGET} "app"
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
    _axl_build_efi(${TARGET} "driver" SOURCES ${ARGN})
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
install -m 755 "$LIBAXL_DIR/scripts/axl-cc" "$PREFIX/bin/axl-cc"
log_info "Installed axl-cc"
if [[ "$BUILD_CPP" == "1" ]]; then
    install -m 755 "$LIBAXL_DIR/scripts/axl-c++" "$PREFIX/bin/axl-c++"
    log_info "Installed axl-c++"
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
