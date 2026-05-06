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

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)      PREFIX="$2"; shift 2 ;;
        --arch)        BUILD_ARCHS="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--prefix DIR] [--arch x64|aa64|all]"
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
    local_prefix="out/native-$arch"
    log_info "Building ($arch, gcc)..."
    make -C "$LIBAXL_DIR" \
        ARCH="$arch" PREFIX="$local_prefix" BUILD=RELEASE \
        ${AXL_TLS:+AXL_TLS=$AXL_TLS} \
        -j "$(nproc)" 2>&1 | tail -3

    mkdir -p "$PREFIX/lib/axl/$arch"
    cp "$LIBAXL_DIR/$local_prefix/lib/libaxl.a"              "$PREFIX/lib/axl/$arch/"
    cp "$LIBAXL_DIR/$local_prefix/build/axl-crt0-native.o"   "$PREFIX/lib/axl/$arch/"
    cp "$LIBAXL_DIR/$local_prefix/build/axl-crt0-minimal.o"  "$PREFIX/lib/axl/$arch/"

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
#   axl_add_app(myapp myapp.c)
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
    set(AXL_GCC_ARCH "")
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

function(axl_add_app TARGET)
    set(SOURCES ${ARGN})

    set(CRT0_OBJS
        "${AXL_GCC_CRT0}" "${AXL_RELOC_OBJ}"
        "${AXL_DEBUG_OBJ}" "${AXL_CRT0_NATIVE}")

    # Compile sources to ELF .o
    set(ALL_OBJS ${CRT0_OBJS})
    foreach(SRC ${SOURCES})
        get_filename_component(SRC_NAME ${SRC} NAME_WE)
        get_filename_component(SRC_ABS ${SRC} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        set(OBJ "${CMAKE_CURRENT_BINARY_DIR}/${SRC_NAME}.o")
        add_custom_command(
            OUTPUT ${OBJ}
            COMMAND ${AXL_CROSS}gcc ${AXL_C_FLAGS} ${AXL_GCC_ARCH}
                    -isystem ${AXL_INCLUDE_DIR}
                    -c ${SRC_ABS} -o ${OBJ}
            DEPENDS ${SRC_ABS}
            COMMENT "gcc: ${SRC}"
        )
        list(APPEND ALL_OBJS ${OBJ})
    endforeach()

    set(SO_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.so")
    set(EFI_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.efi")

    # Link to ELF shared object. --no-warn-rwx-segments suppresses a
    # false-positive linker warning — the .so is an intermediate consumed
    # by objcopy → PE/COFF, no OS loads it, and the resulting .efi has
    # properly split per-section permissions.
    add_custom_command(
        OUTPUT ${SO_FILE}
        COMMAND ${AXL_CROSS}ld -nostdlib -shared -Bsymbolic
                --no-warn-rwx-segments
                -T ${AXL_EFI_LDS}
                -o ${SO_FILE}
                ${ALL_OBJS}
                ${AXL_LIB_DIR}/libaxl.a
        DEPENDS ${ALL_OBJS}
        COMMENT "ld: ${TARGET}.so"
    )

    # Convert to PE/COFF + patch debug directory
    add_custom_command(
        OUTPUT ${EFI_FILE}
        COMMAND ${AXL_CROSS}objcopy
                -j .text -j .sdata -j .data -j .dynamic -j .dynsym
                -j .rel -j .rela -j .reloc -j .rodata -j .dbgdir
                --output-target=${AXL_PE_TARGET} --subsystem=10
                ${SO_FILE} ${EFI_FILE}
        COMMAND ${AXL_PE_SET_DEBUG} ${EFI_FILE}
        DEPENDS ${SO_FILE}
        COMMENT "objcopy+pe-set-debug: ${TARGET}.efi"
    )

    add_custom_target(${TARGET} ALL
        DEPENDS ${EFI_FILE}
    )
endfunction()
CMAKE_SUPPORT

log_info "Installed axl-config.cmake"

# ---------------------------------------------------------------------------
# Generate axl-cc
# ---------------------------------------------------------------------------

cat > "$PREFIX/bin/axl-cc" << 'NATIVE_WRAPPER'
#!/bin/bash
# axl-cc — compile C source to UEFI .efi binary
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

ARCH="x64"
TYPE="app"
BUILD="release"
RUNTIME="full"
ENTRY=""
OUTPUT=""
VERBOSE=false
RUN=false
RUN_ARGS=()
SOURCES=()
CFLAGS_EXTRA=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)     ARCH="$2"; shift 2 ;;
        --type)     TYPE="$2"; shift 2 ;;
        --entry)    ENTRY="$2"; shift 2 ;;
        --debug)    BUILD="debug"; shift ;;
        --release)  BUILD="release"; shift ;;
        --minimal-runtime) RUNTIME="minimal"; shift ;;
        --run)      RUN=true; shift
                    # Remaining args after --run are passed to run-qemu.sh
                    while [[ $# -gt 0 ]]; do
                        RUN_ARGS+=("$1"); shift
                    done ;;
        -o)         OUTPUT="$2"; shift 2 ;;
        --verbose|-v) VERBOSE=true; shift ;;
        -I|-D|-include)
            CFLAGS_EXTRA+=("$1" "$2"); shift 2 ;;
        -I*|-D*|-W*|-Wno-*)
            CFLAGS_EXTRA+=("$1"); shift ;;
        -f*)
            CFLAGS_EXTRA+=("$1"); shift ;;
        --version)
            VER=$(cat "$SDK_DIR/share/axl/version" 2>/dev/null || echo "unknown")
            DATE=$(cat "$SDK_DIR/share/axl/build-date" 2>/dev/null || echo "unknown")
            echo "axl-cc $VER (gcc, built $DATE)"
            exit 0 ;;
        -h|--help)
            cat <<HELP
axl-cc -- compile C source to UEFI binary (.efi)

Usage: axl-cc [OPTIONS] source.c [source2.c ...] [-o output.efi]
       axl-cc [OPTIONS] source.c --run [run-qemu args...]

Options:
  --arch x64|aa64       Target architecture (default: x64)
  --type app|driver|runtime  Image type (default: app)
  --entry NAME          Custom entry point for drivers
  --debug               Debug build (-Og, DWARF, leak tracking)
  --release             Release build (-Os, DWARF, no leak tracking) [default]
  --minimal-runtime     Link against axl-crt0-minimal.o (no registry,
                        no atexit, no signal notify, no default loop).
                        App-type only. See docs/AXL-Lifecycle.md §10.4.
  --run                 Build and run in QEMU (remaining args passed to run-qemu.sh)
  -o FILE               Output filename (default: <source>.efi)
  -I DIR                Add include search path
  -DNAME[=VALUE]        Define preprocessor macro
  -Wfoo / -Wno-foo      Warning flags
  -ffoo                 Compiler flags
  -v, --verbose         Print compiler/linker commands
  --version             Show version and build info
  -h, --help            Show this help

Examples:
  axl-cc hello.c -o hello.efi
  axl-cc --debug hello.c -o hello.efi
  axl-cc hello.c --run
  axl-cc hello.c --run --net --timeout 30

Toolchain: gcc
HELP
            exit 0 ;;
        *)      SOURCES+=("$1"); shift ;;
    esac
done

if [[ ${#SOURCES[@]} -eq 0 ]]; then
    echo "Usage: axl-cc [OPTIONS] source.c [-o output.efi]  (try --help)" >&2
    exit 1
fi

for src in "${SOURCES[@]}"; do
    if [[ ! -f "$src" ]]; then
        echo "ERROR: source file not found: $src" >&2
        exit 1
    fi
done

[[ -z "$OUTPUT" ]] && OUTPUT="${SOURCES[0]%.c}.efi"

LIB_DIR="$SDK_DIR/lib/axl/$ARCH"
if [[ ! -d "$LIB_DIR" ]]; then
    echo "ERROR: no SDK libraries for arch '$ARCH' in $LIB_DIR" >&2
    exit 1
fi

# Image type → entry + CRT0
case "$TYPE" in
    app)     ENTRY="${ENTRY:-_AxlEntry}" ;;
    driver)  ENTRY="${ENTRY:-DriverEntry}" ;;
    runtime) ENTRY="${ENTRY:-DriverEntry}" ;;
    *)  echo "ERROR: unknown type '$TYPE' (use: app, driver, runtime)" >&2; exit 1 ;;
esac

run_cmd() {
    if [[ "$VERBOSE" == "true" ]]; then
        echo "+ $*" >&2
    fi
    "$@"
}

# Build mode flags. Both modes carry DWARF debug info — release
# trades only the leak tracking and goes to -Os, debug stays at
# -Og + AXL_MEM_DEBUG. The .efi itself stays slim either way (debug
# DWARF lives in the side-by-side .debug file referenced by the PE
# debug data directory; pe-set-debug wires this up at link time).
if [[ "$BUILD" == "debug" ]]; then
    OPT_FLAGS="-Og -g -gdwarf -DAXL_MEM_DEBUG"
else
    OPT_FLAGS="-Os -g -gdwarf -DNDEBUG"
fi

if [[ "$VERBOSE" == "true" ]]; then
    echo "[axl-cc] arch=$ARCH type=$TYPE build=$BUILD toolchain=gcc entry=$ENTRY output=$OUTPUT" >&2
fi

TMPDIR="/tmp/axl-cc-$$"
mkdir -p "$TMPDIR"

# Architecture-specific settings
case "$ARCH" in
    x64)  CROSS=""; PE_TARGET="pei-x86-64"
          GCC_ARCH="-mno-red-zone -march=x86-64"
          EFI_LDS="$SDK_DIR/lib/axl/elf_x86_64_efi.lds"
          GCC_CRT0="$LIB_DIR/axl-crt0-gcc-x86_64.o" ;;
    aa64) CROSS="aarch64-linux-gnu-"; PE_TARGET="pei-aarch64-little"
          GCC_ARCH=""
          EFI_LDS="$SDK_DIR/lib/axl/elf_aarch64_efi.lds"
          GCC_CRT0="$LIB_DIR/axl-crt0-gcc-aarch64.o" ;;
esac

RELOC_OBJ="$LIB_DIR/axl-reloc.o"
DEBUG_INFO_OBJ="$LIB_DIR/axl-debug-info.o"
PE_SET_DEBUG="$SDK_DIR/bin/pe-set-debug"

# Compile
OBJS=()
for src in "${SOURCES[@]}"; do
    obj="$TMPDIR/$(basename "${src%.c}").o"

    run_cmd ${CROSS}gcc \
        -ffreestanding -fshort-wchar -fno-builtin \
        -fno-stack-protector -fno-omit-frame-pointer -fpic $GCC_ARCH \
        -ffunction-sections -fdata-sections \
        $OPT_FLAGS -Wall \
        -DAXL_BACKEND_NATIVE \
        -isystem "$SDK_DIR/include/axl-sdk" \
        ${CFLAGS_EXTRA[@]+"${CFLAGS_EXTRA[@]}"} \
        -c "$src" -o "$obj" || { echo "ERROR: compilation failed: $src" >&2; rm -rf "$TMPDIR"; exit 1; }

    OBJS+=("$obj")
done

# Link
# App:    asm CRT0 → _AxlEntry (C CRT0 axl-crt0-native.o) → main
# App (--minimal-runtime):
#         asm CRT0 → _AxlEntry (C CRT0 axl-crt0-minimal.o) → main
#         Minimal CRT0 skips registry/atexit/signal/default-loop setup.
# Driver: asm CRT0 → _AxlEntry (aliased to user's $ENTRY via --defsym)
#
# Drivers can't link a C CRT0 (they pull in main()) and don't define
# _AxlEntry directly, so we tell the linker that _AxlEntry is just
# another name for the user's entry function (e.g. DriverEntry). This
# keeps the asm CRT0's relocation step in the call path.
LDFLAGS_EXTRA=()
if [[ "$TYPE" == "app" ]]; then
    if [[ "$RUNTIME" == "minimal" ]]; then
        C_CRT0="$LIB_DIR/axl-crt0-minimal.o"
    else
        C_CRT0="$LIB_DIR/axl-crt0-native.o"
    fi
    CRT0_OBJS="$GCC_CRT0 $RELOC_OBJ $DEBUG_INFO_OBJ $C_CRT0"
else
    if [[ "$RUNTIME" == "minimal" ]]; then
        echo "ERROR: --minimal-runtime is app-type only (use on --type app)" >&2
        rm -rf "$TMPDIR"
        exit 1
    fi
    CRT0_OBJS="$GCC_CRT0 $RELOC_OBJ $DEBUG_INFO_OBJ"
    LDFLAGS_EXTRA+=("--defsym=_AxlEntry=$ENTRY")
fi

# PE/COFF subsystem code: 10=APPLICATION, 11=BOOT_SERVICE_DRIVER, 12=RUNTIME_DRIVER.
# Loaded via the wrong path the firmware misroutes the entry point, leading to
# crashes (KVM emulation failure / silent dispatch failures).
case "$TYPE" in
    app)     SUBSYSTEM=10 ;;
    driver)  SUBSYSTEM=11 ;;
    runtime) SUBSYSTEM=12 ;;
esac

SO_FILE="$TMPDIR/output.so"
# --no-warn-rwx-segments: the .so is just an intermediate consumed by
# objcopy → PE/COFF; no OS loads it, so the linker's RWX warning is a
# false positive (the .efi has properly split per-section permissions).
run_cmd ${CROSS}ld -nostdlib -shared -Bsymbolic --no-warn-rwx-segments \
    -T "$EFI_LDS" \
    ${LDFLAGS_EXTRA[@]+"${LDFLAGS_EXTRA[@]}"} \
    -o "$SO_FILE" \
    $CRT0_OBJS \
    "${OBJS[@]}" \
    "$LIB_DIR/libaxl.a" || { echo "ERROR: linking failed" >&2; rm -rf "$TMPDIR"; exit 1; }

run_cmd ${CROSS}objcopy \
    -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
    -j .rel -j .rela -j .reloc -j .rodata -j .dbgdir \
    --output-target="$PE_TARGET" --subsystem=$SUBSYSTEM \
    "$SO_FILE" "$OUTPUT" || { echo "ERROR: objcopy failed" >&2; rm -rf "$TMPDIR"; exit 1; }

# Patch PE debug directory with module name
"$PE_SET_DEBUG" "$OUTPUT" "$(basename "$OUTPUT")" || {
    echo "ERROR: pe-set-debug failed" >&2; rm -rf "$TMPDIR"; exit 1;
}

# Keep the ELF .so alongside the .efi (has DWARF symbols for addr2line/debugging)
cp "$SO_FILE" "${OUTPUT%.efi}.so"

rm -rf "$TMPDIR"
echo "$OUTPUT"

# --run: launch in QEMU
if [[ "$RUN" == "true" ]]; then
    # Find run-qemu.sh relative to the SDK or in the source tree
    RUN_SCRIPT=""
    if [[ -f "$SDK_DIR/scripts/run-qemu.sh" ]]; then
        RUN_SCRIPT="$SDK_DIR/scripts/run-qemu.sh"
    elif [[ -f "$SCRIPT_DIR/../../scripts/run-qemu.sh" ]]; then
        RUN_SCRIPT="$SCRIPT_DIR/../../scripts/run-qemu.sh"
    fi

    if [[ -z "$RUN_SCRIPT" ]]; then
        echo "ERROR: run-qemu.sh not found (--run requires the AXL source tree)" >&2
        exit 1
    fi

    # Map axl-cc arch names to run-qemu arch names
    case "$ARCH" in
        x64)  QEMU_ARCH="X64" ;;
        aa64) QEMU_ARCH="AARCH64" ;;
        *)    QEMU_ARCH="$ARCH" ;;
    esac

    exec "$RUN_SCRIPT" --arch "$QEMU_ARCH" ${RUN_ARGS[@]+"${RUN_ARGS[@]}"} "$OUTPUT"
fi
NATIVE_WRAPPER

chmod +x "$PREFIX/bin/axl-cc"
log_info "Installed axl-cc"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

log_info "AXL SDK installed to $PREFIX (toolchain: gcc)"
echo ""
echo "  Build app:    $PREFIX/bin/axl-cc hello.c -o hello.efi"
echo "  Build driver: $PREFIX/bin/axl-cc --type driver mydrv.c -o mydrv.efi"
echo "  CMake:        find_package(axl REQUIRED)"
echo "                axl_add_app(hello hello.c)"
