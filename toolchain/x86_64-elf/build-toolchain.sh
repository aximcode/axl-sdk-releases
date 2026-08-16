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
# OUR build revision on top of those upstream versions. The install prefix and
# the published artifact carry it, so a rebuild with different configure flags
# is distinguishable from upstream 14.3.0 -- which is exactly what -axl marks
# (the PE-target enable below). check-toolchain-conf compares the manifest
# against GCC_VER + this.
#
# BUMP THIS whenever a configure flag here changes. The flags are not recorded
# anywhere in the installed tree that a consumer can compare against, so two
# trees with the same version string and different flags are indistinguishable
# until something silently misbehaves -- which is exactly what -axl2 marks
# (the init-array enable below, whose absence made every global constructor
# silently not run).
AXL_REV="${AXL_REV:--axl2}"
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
PREFIX="${PREFIX:-$HERE/out/$TARGET-gcc-$GCC_VER$AXL_REV}"
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
    # --enable-targets=x86_64-pep: WITHOUT it, a --target=x86_64-elf binutils
    # carries only elf64-x86-64 and elf32-i386, and `objcopy
    # --output-target=pei-x86-64` -- the step that turns AXL's .so into a .efi
    # -- fails outright. That is the single reason the tree still links x64
    # with the HOST binutils; ARM's aarch64 toolchain ships pei-aarch64-little
    # already, so only x64 is affected. pep is PE+ (64-bit PE), which is what
    # pei-x86-64 is; the 32-bit x86_64-pe comes with it.
    ( cd "$BUILD/binutils" && "$SRC/binutils/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --enable-targets=x86_64-pep \
        --with-sysroot --disable-nls --disable-werror \
      && make -j"$JOBS" "${MAKEINFO_STUB[@]}" \
      && make install "${MAKEINFO_STUB[@]}" )
fi

# --- 2. GCC stage 1: the compiler and libgcc -----------------------------
# --with-newlib tells GCC the target libc is newlib rather than glibc.
# --disable-threads/--disable-tls are what turn libsupc++'s exception
# globals from a __thread variable into a plain static, and what keep the
# stack protector off the %fs:0x28 slot UEFI does not provide.
#
# --enable-initfini-array: global constructors go in .init_array, which is what
# AXL's crt0 walks (src/runtime/axl-cxxabi.c), rather than the legacy .ctors.
# It has to be asked for. GCC's autoconf probe for it LINKS a test program,
# which a cross build without a libc cannot do, so it falls back to the target
# default -- and for x86_64-*-elf that default is off (HAVE_INITFINI_ARRAY_
# SUPPORT 0 in auto-host.h). aarch64 reaches .init_array anyway because the
# port forces it at the target level, which is why aa64 never showed this and
# x64 did.
#
# The failure it prevents is SILENT and total: the first -axl toolchain put
# every constructor in .ctors, AXL walked an empty .init_array, and nothing
# ran -- not the fixture's, and not the 26 objects' worth inside the
# toolchain's own libstdc++.a (libsupc++'s emergency exception pool among
# them). The verify block below asserts it rather than trusting the flag.
if [ ! -x "$PREFIX/bin/$TARGET-gcc" ]; then
    log "GCC $GCC_VER stage 1 (compiler + libgcc)"
    rm -rf "$BUILD/gcc"; mkdir -p "$BUILD/gcc"
    ( cd "$BUILD/gcc" && "$SRC/gcc/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --with-newlib --without-headers \
        --disable-threads --disable-tls \
        --enable-initfini-array \
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

# Global constructors must land in .init_array, the only list AXL's crt0 walks
# (src/runtime/axl-cxxabi.c). Asserted in TWO places because they fail
# independently: --enable-initfini-array could be dropped from the configure
# above, and a future GCC could change the target default underneath it.
#
# Compiling is not enough on its own -- the fixture proves what the COMPILER
# does today, and libstdc++.a proves what it did when the shipped archive was
# built. A mismatch between those two is a stale-stage-2 build, which the
# `[ ! -f libsupc++.a ]` guards above make reachable.
#
# There is no partial credit here: a constructor in .ctors does not run, is not
# diagnosed, and leaves no trace. libsupc++'s emergency exception pool is one.
log "verify .init_array"
_iatmp="$(mktemp -d)"
trap 'rm -rf "$_iatmp"' EXIT
printf 'struct S { S(); ~S(); };\nS s;\n' > "$_iatmp/ctor.cpp"
"$PREFIX/bin/$TARGET-g++" -c "$_iatmp/ctor.cpp" -o "$_iatmp/ctor.o"
_secs=$("$PREFIX/bin/$TARGET-readelf" -SW "$_iatmp/ctor.o")
_ia=$(printf '%s\n' "$_secs" | grep -c '\.init_array' || true)
_ct=$(printf '%s\n' "$_secs" | grep -c '\.ctors' || true)
printf '  compiler emits: .init_array %s   .ctors %s\n' "$_ia" "$_ct"
if [ "$_ia" = "0" ] || [ "$_ct" != "0" ]; then
    echo "  FAIL: constructors are not going to .init_array -- they would" >&2
    echo "        silently never run. Is --enable-initfini-array still in" >&2
    echo "        the GCC configure above?" >&2
    exit 1
fi

# Same question of the shipped archive. Counted over SECTION HEADERS rather
# than objects: one .ctors section anywhere is one constructor that will not
# run, and the count is the useful number to print when it is not zero.
#
# The EXISTENCE check is not belt-and-braces. `readelf 2>/dev/null | grep -c`
# on a missing file yields 0, which reads as "no .ctors" -- so without this the
# assertion PASSES on precisely the failed stage-2 build it exists to catch.
# The verify loop above only PRINTS "MISSING" for that case.
_libstdcxx="$PREFIX/$TARGET/lib/libstdc++.a"
if [ ! -f "$_libstdcxx" ]; then
    echo "  FAIL: $_libstdcxx does not exist -- stage 2 did not produce a" >&2
    echo "        libstdc++, so there is nothing to assert about." >&2
    exit 1
fi
_lib_ct=$("$PREFIX/bin/$TARGET-readelf" -SW "$_libstdcxx" \
              2>/dev/null | grep -c '\.ctors' || true)
printf '  libstdc++ .ctors sections: %s\n' "$_lib_ct"
if [ "$_lib_ct" != "0" ]; then
    echo "  FAIL: libstdc++.a carries .ctors, so it was built by a compiler" >&2
    echo "        without --enable-initfini-array. Stage 2 is stale: remove" >&2
    echo "        $PREFIX and rebuild." >&2
    exit 1
fi

# STRIP. A gcc built from source keeps its debug info: 1.5 GB installed, of
# which cc1 alone is 326 MB. Stripping takes that to 235 MB with no loss of
# function -- measured: the stripped toolchain builds all 43 AXL test images
# and AxlTestData runs 2078/0 with no leaks. ARM ships theirs stripped too
# (their tree drops only 6.6% under the same pass, i.e. it is already done),
# so this brings us in line rather than off it.
#
# ONLY executables and shared objects. Target libraries (libgcc.a, libc.a,
# crt*.o) must keep their symbol tables or nothing links against them -- the
# `file` test excludes them, since an archive is not an ELF executable.
log "stripping (executables and shared objects only)"
_before=$(du -sh "$PREFIX" | cut -f1)
find "$PREFIX" -type f -exec sh -c '
    file -b "$1" | grep -q "ELF.*executable\|ELF.*shared object" &&
        strip --strip-unneeded "$1" 2>/dev/null' _ {} \;
log "stripped: $_before -> $(du -sh "$PREFIX" | cut -f1)"

# PACKAGE (opt-in). Produces the tarball published as a release artifact, so
# every other machine downloads 55 MB instead of spending 40 minutes here.
# Built from what is on disk rather than through a separate pipeline, so the
# artifact cannot drift from a local install.
if [ "${PACKAGE:-0}" = "1" ]; then
    _tarball="$(dirname "$PREFIX")/$(basename "$PREFIX").tar.xz"
    log "packaging -> $_tarball"
    tar -C "$(dirname "$PREFIX")" -cf - "$(basename "$PREFIX")" \
        | xz -T0 -6 > "$_tarball"
    sha256sum "$_tarball" | tee "$_tarball.sha256"
    log "package size: $(du -h "$_tarball" | cut -f1)"
fi

log "done -> $PREFIX"
