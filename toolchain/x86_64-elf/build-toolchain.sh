#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
#
# Build an x86_64-elf bare-metal toolchain: binutils + GCC + newlib +
# a newlib-configured libstdc++/libsupc++.
#
# WHY THIS EXISTS
# ---------------
# aa64 gets its toolchain from ARM (scripts/install-arm-toolchain.sh
# fetches aarch64-none-elf). Nobody publishes the x86_64 equivalent:
# Homebrew's x86_64-elf-gcc is a bare compiler with no libc at all (no
# newlib formula, no newlib dependency), and bootlin ships only
# linux-gnu/musl. So x64 has been borrowing the HOST's glibc-targeted
# g++, and that is the root of three separate problems measured in
# docs/AXL-Cxx-Design.md and docs/AXL-Cxx-Unwinder-Design.md:
#
#   - libsupc++ keeps __cxa_eh_globals in __thread storage, read through
#     %fs, which UEFI never sets up -- so the first `throw` jumps through
#     a garbage pointer before it ever reaches the unwinder.
#   - 18 further %fs:0x28 accesses across 11 objects are glibc's STACK
#     CANARY, which UEFI likewise never sets up.
#   - libstdc++'s headers are configured for glibc, so `--hosted` is a
#     workaround for borrowing them rather than a feature.
#
# A --with-newlib --disable-threads --disable-tls build has none of
# those: the ARM toolchain's libsupc++ measures 0 TLS symbols and 0
# thread-pointer reads across all 65 objects. This script produces the
# x86_64 counterpart so both arches are built the same way.
#
# SCOPE
# -----
# Deliberately a separate subdirectory. It may become its own project --
# a toolchain has its own release cadence, its own host matrix and its
# own reproducibility concerns, none of which belong in the SDK's build.
# Nothing here is wired into the SDK's Makefile.

set -euo pipefail

# --- versions ------------------------------------------------------------
# GCC matches the host (14.3.1) and ARM's aarch64-none-elf (14.3.1), so a
# consumer sees one compiler generation across both arches.
GCC_VER="${GCC_VER:-14.3.0}"
BINUTILS_VER="${BINUTILS_VER:-2.43}"
NEWLIB_VER="${NEWLIB_VER:-4.4.0.20231231}"

TARGET="x86_64-elf"
JOBS="${JOBS:-$(nproc)}"

# AXL links every image with `ld -shared` -- the intermediate ELF that
# objcopy turns into PE/COFF -- so every TARGET library must be built
# position-independent. Without this, libstdc++.a's eh_alloc.o carries an
# R_X86_64_32 against .bss and the link dies with "can not be used when
# making a shared object; recompile with -fPIC".
#
# aa64 does not hit this: AArch64 codegen is position-independent for
# these relocations, so ARM's stock toolchain links fine. x86-64 is the
# arch where it has to be asked for explicitly.
TARGET_FLAGS="-O2 -fPIC"

# binutils and GCC build their own .info manuals, which needs texinfo.
# It is not a runtime dependency of anything we ship and is frequently
# absent -- the miss surfaces as a bare "Error 127" deep in bfd/doc,
# which reads like a compiler failure and is not.
#
# This MUST be passed on the make COMMAND LINE, not exported. Exporting
# it satisfies the top-level configure, but each sub-configure re-detects
# makeinfo and overwrites it with the tree's `missing` wrapper, which
# then exits 127. A command-line variable overrides both.
MAKEINFO_STUB=(MAKEINFO=true)

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HERE/out/$TARGET-gcc-$GCC_VER}"
WORK="${WORK:-$HERE/work}"
SRC="$WORK/src"
BUILD="$WORK/build"

GNU_MIRROR="${GNU_MIRROR:-https://ftp.gnu.org/gnu}"
NEWLIB_URL="${NEWLIB_URL:-ftp://sourceware.org/pub/newlib}"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

mkdir -p "$SRC" "$BUILD" "$PREFIX"
export PATH="$PREFIX/bin:$PATH"

# --- fetch ---------------------------------------------------------------
fetch() {
    local url="$1" out="$2"
    [ -f "$out" ] && { log "have $(basename "$out")"; return 0; }
    log "fetch $(basename "$out")"
    curl -fL --retry 3 --progress-bar -o "$out.part" "$url"
    mv "$out.part" "$out"
}

fetch "$GNU_MIRROR/binutils/binutils-$BINUTILS_VER.tar.xz" "$SRC/binutils.tar.xz"
fetch "$GNU_MIRROR/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"   "$SRC/gcc.tar.xz"
fetch "https://sourceware.org/pub/newlib/newlib-$NEWLIB_VER.tar.gz" "$SRC/newlib.tar.gz"

untar() {
    local tarball="$1" dir="$2"
    [ -d "$dir" ] && { log "have $(basename "$dir")"; return 0; }
    log "unpack $(basename "$tarball")"
    mkdir -p "$dir"
    tar -xf "$tarball" -C "$dir" --strip-components=1
}

untar "$SRC/binutils.tar.xz" "$SRC/binutils"
untar "$SRC/gcc.tar.xz"      "$SRC/gcc"
untar "$SRC/newlib.tar.gz"   "$SRC/newlib"

log "GCC prerequisites (gmp/mpfr/mpc)"
( cd "$SRC/gcc" && [ -d gmp ] || ./contrib/download_prerequisites )

# --- 1. binutils ---------------------------------------------------------
if [ ! -x "$PREFIX/bin/$TARGET-ld" ]; then
    log "binutils $BINUTILS_VER"
    rm -rf "$BUILD/binutils"; mkdir -p "$BUILD/binutils"
    ( cd "$BUILD/binutils" && "$SRC/binutils/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --with-sysroot --disable-nls --disable-werror \
      && make -j"$JOBS" "${MAKEINFO_STUB[@]}" \
      && make install "${MAKEINFO_STUB[@]}" )
fi

# --- 2. GCC stage 1: the compiler and libgcc -----------------------------
# --with-newlib tells GCC the target libc is newlib rather than glibc.
# --disable-threads/--disable-tls are what turn libsupc++'s exception
# globals from a __thread variable into a plain static, and what keep the
# stack protector off the %fs:0x28 slot UEFI does not provide.
if [ ! -x "$PREFIX/bin/$TARGET-gcc" ]; then
    log "GCC $GCC_VER stage 1 (compiler + libgcc)"
    rm -rf "$BUILD/gcc"; mkdir -p "$BUILD/gcc"
    ( cd "$BUILD/gcc" && "$SRC/gcc/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --with-newlib --without-headers \
        --disable-threads --disable-tls \
        --disable-nls --disable-shared --disable-libssp \
        --disable-libgomp --disable-libquadmath --disable-libatomic \
        --enable-languages=c,c++ \
        CFLAGS_FOR_TARGET="$TARGET_FLAGS" \
        CXXFLAGS_FOR_TARGET="$TARGET_FLAGS" \
      && make -j"$JOBS" "${MAKEINFO_STUB[@]}" all-gcc \
      && make -j"$JOBS" "${MAKEINFO_STUB[@]}" all-target-libgcc \
      && make "${MAKEINFO_STUB[@]}" install-gcc install-target-libgcc )
fi

# --- 3. newlib -----------------------------------------------------------
if [ ! -f "$PREFIX/$TARGET/lib/libc.a" ]; then
    log "newlib $NEWLIB_VER"
    rm -rf "$BUILD/newlib"; mkdir -p "$BUILD/newlib"
    ( cd "$BUILD/newlib" && "$SRC/newlib/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --disable-newlib-supplied-syscalls \
        CFLAGS_FOR_TARGET="$TARGET_FLAGS" \
      && make -j"$JOBS" "${MAKEINFO_STUB[@]}" \
      && make install "${MAKEINFO_STUB[@]}" )
fi

# --- 4. GCC stage 2: libstdc++ and libsupc++ against newlib --------------
# This is the step that produces what x64 has never had: a libstdc++ and
# libsupc++ configured for a freestanding target.
if [ ! -f "$PREFIX/$TARGET/lib/libsupc++.a" ]; then
    log "GCC stage 2 (libstdc++ / libsupc++ against newlib)"
    ( cd "$BUILD/gcc" \
      && make -j"$JOBS" "${MAKEINFO_STUB[@]}" all-target-libstdc++-v3 \
      && make "${MAKEINFO_STUB[@]}" install-target-libstdc++-v3 )
fi

# --- verify --------------------------------------------------------------
log "verify"
"$PREFIX/bin/$TARGET-gcc" --version | head -1 | sed 's/^/  /'
for f in libc.a libsupc++.a libstdc++.a; do
    p="$PREFIX/$TARGET/lib/$f"
    [ -f "$p" ] && printf '  %-14s %s\n' "$f" "$(du -h "$p" | cut -f1)" \
                || printf '  %-14s MISSING\n' "$f"
done

# The whole point: no thread-local storage and no glibc stack canary.
tls=$("$PREFIX/bin/$TARGET-readelf" -s "$PREFIX/$TARGET/lib/libsupc++.a" 2>/dev/null | grep -c ' TLS ' || true)
fs=$("$PREFIX/bin/$TARGET-objdump" -d "$PREFIX/$TARGET/lib/libsupc++.a" 2>/dev/null | grep -cE '%fs:' || true)
printf '  libsupc++ TLS symbols: %s   %%fs accesses: %s\n' "$tls" "$fs"
if [ "$tls" != "0" ] || [ "$fs" != "0" ]; then
    echo "  FAIL: the build still depends on TLS or the glibc canary" >&2
    exit 1
fi

# Absolute relocations in a target library mean a non-PIC build, which
# AXL's `ld -shared` step rejects. Checked here rather than discovered at
# the consumer's link.
#
# DEBUG SECTIONS MUST BE EXCLUDED. .debug_info and friends use absolute
# relocations legitimately and never reach the image -- ARM's stock
# libgcc.a, which links fine on aa64, carries 53480 of them in
# .debug_info alone. A naive `grep -c R_X86_64_32` counts those and fails
# a perfectly good build; only allocatable sections matter.
abs=$("$PREFIX/bin/$TARGET-readelf" -r "$PREFIX/$TARGET/lib/libstdc++.a" 2>/dev/null | awk '
    /^Relocation section/ { sec = $3 }
    /R_X86_64_32/ && sec !~ /debug/ { n++ }
    END { print n + 0 }')
printf '  libstdc++ absolute (non-PIC) relocations: %s\n' "$abs"
if [ "$abs" != "0" ]; then
    echo "  FAIL: target libraries are not position-independent" >&2
    exit 1
fi
log "done -> $PREFIX"
