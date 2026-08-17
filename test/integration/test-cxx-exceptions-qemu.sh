#!/bin/bash
# test-meta: arch=both needs= est=70 local-only=0
# test-cxx-exceptions-qemu.sh — real try/catch under UEFI, and the price a C
# image does NOT pay for it.
#
# `axl-c++ -fexceptions` is the whole opt-in. axl-cc sees the flag (or an input
# object referencing __gxx_personality_v0, so a staged build works too) and
# switches three things at once: the exceptions linker script, which KEEPs
# .eh_frame and defines __eh_frame_start; two extra `objcopy -j` entries, so
# the tables actually reach the image; and the toolchain's libstdc++ /
# libsupc++ / libgcc plus AXL's glue objects INSTEAD of libaxl-cxx.a.
#
# What this asserts, and why each is here rather than implied:
#
#   1. The image RUNS, with exact output. Seven cases: a global constructor
#      that throws and catches BEFORE main (which is what proves the frame
#      table is registered ahead of the .init_array walk, not from main), a
#      catch by exact type across three frames, every destructor on the unwind
#      path running EXACTLY once, a rethrow preserving the exception object,
#      catch(...), a non-matching handler declining, and container elements
#      destructing mid-unwind.
#   2. Both arches. They differ in toolchain provenance (ours vs ARM's) and in
#      relocation handling, and aa64 has diverged there before -- an RTTI link
#      once produced a split DT_RELA the crt0 walked off the end of.
#   3. THE BYTE-IDENTITY CONSTRAINT (AXL-Cxx-Unwinder-Design.md §U2): a C image
#      must be unchanged whether or not the SDK supports exceptions. Measured
#      when that was designed: putting KEEP(*(.eh_frame)) in the SHARED linker
#      script costs a C-only image +11.5-13.6 KB, +16.8%, for tables it can
#      never use. So the C image must carry NO unwind sections, and the shared
#      scripts must contain no KEEP of them. Asserted rather than trusted,
#      because the tempting simplification -- one linker script, KEEP always --
#      is invisible until someone measures an image.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-exceptions-qemu.sh [X64|AARCH64|both]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

AXL_CXX="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-c++"
AXL_CC="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-cc"
SRC="$SCRIPT_DIR/cxx-exceptions-selftest.cpp"
C_SRC="$PROJECT_DIR/sdk/examples/hello.c"
WORK="$(mktemp -d -t axl-cxx-eh.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

EXPECTED=(
    "  PASS: a global constructor threw and caught, before main"
    "  PASS: caught by exact type across three frames"
    "  dtors=3"
    "  PASS: every destructor on the unwind path ran exactly once"
    "  PASS: a rethrow preserves the original exception object"
    "  PASS: catch(...) catches a non-class type"
    "  PASS: a non-matching handler does not intercept"
    "  vec_dtors=3"
    "  PASS: container elements destruct during unwind"
    "=== 7 passed, 0 failed ==="
)

pass=0
fail=0
skipped=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "  PASS: $2"; pass=$((pass + 1))
    else echo "  FAIL: $2"; fail=$((fail + 1)); fi
}

# The unwind sections, by the exact names objcopy takes and the linker scripts
# emit. Read off the PE rather than the intermediate .so, so this sees what
# SHIPS.
#
# Prints a COUNT, or `ERR` when the image could not be read at all -- and that
# distinction is the whole point. The first version returned a boolean from
# `objdump -h | grep -q`, which conflates "no unwind sections" with "objdump
# could not open this file". The HOST objdump cannot read pei-aarch64-little
# ("file format not recognized"), so on aa64 every negative assertion below was
# passing while blind, and the positive one failed on an image that in fact
# carried both sections. Hence the CROSS objdump, and hence the .text control:
# any readable image has one, so its absence means we are not looking at an
# image at all.
eh_section_count() {  # eh_section_count <efi> <objdump> -> count | ERR
    local out
    out="$("$2" -h "$1" 2>/dev/null)" || { echo ERR; return 0; }
    grep -q '\.text' <<<"$out"        || { echo ERR; return 0; }
    grep -cE '\.eh_frame|\.gcc_except_table' <<<"$out"
}

run_one() {
    local arch="$1" cc_arch objdump rc n
    # The CROSS objdump, from the same manifest every other consumer reads.
    # The host's cannot read pei-aarch64-little at all.
    # shellcheck source=/dev/null
    . "$PROJECT_DIR/scripts/axl-toolchains.conf"
    case "$arch" in
        X64)     cc_arch="x64"
                 objdump="${AXL_X64_BINUTILS_PREFIX:-$AXL_X64_BINUTILS_PREFIX_DEFAULT}objdump" ;;
        AARCH64) cc_arch="aa64"
                 objdump="${AXL_AA64_BINUTILS_PREFIX:-$AXL_AA64_BINUTILS_PREFIX_DEFAULT}objdump" ;;
    esac

    echo "=== cxx-exceptions ($arch) ==="
    if [[ ! -x "$AXL_CXX" || ! -f "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/lib/axl/$cc_arch/axl-cxxrt-eh.o" ]]; then
        echo "  SKIP ($arch): no staged C++ SDK with the exceptions glue"
        echo "        run: scripts/install.sh --arch $cc_arch --cpp"
        skipped=$((skipped + 1))
        return
    fi

    # -----------------------------------------------------------------
    # 1. Build with -fexceptions.
    # -----------------------------------------------------------------
    local efi="$WORK/eh-$cc_arch.efi"
    "$AXL_CXX" --arch "$cc_arch" --release -fexceptions "$SRC" -o "$efi" \
        >"$WORK/build-$cc_arch.log" 2>&1
    rc=$?
    check "$rc" "$arch: axl-c++ -fexceptions builds (rc=$rc)"
    if [[ "$rc" -ne 0 ]]; then
        grep -E "undefined reference|error" "$WORK/build-$cc_arch.log" \
            | head -5 | sed 's/^/      /'
        return
    fi

    # A SUCCEEDING link must also be a QUIET one. Borrowing libnosys.a for the
    # newlib syscall stubs made every exceptions link emit ten "not implemented
    # and will always fail" warnings, on a build that worked -- noise a
    # consumer has to learn to ignore. AXL supplies them instead
    # (src/cxxrt/axl-cxxrt-stubs.c); this is what keeps that true.
    ! grep -q "not implemented and will always fail" "$WORK/build-$cc_arch.log"
    check "$?" "$arch: the exceptions link emits no libnosys warnings"

    # -----------------------------------------------------------------
    # 2. The unwind tables REACHED the image.
    # -----------------------------------------------------------------
    # objcopy takes EXACT section names and silently drops anything not in its
    # -j list, so the tables can be linked correctly and then thrown away. That
    # failure surfaces only at runtime, on the first throw.
    n="$(eh_section_count "$efi" "$objdump")"
    [[ "$n" != "ERR" && "$n" -ge 1 ]]
    check "$?" "$arch: the .efi carries .eh_frame / .gcc_except_table (found $n)"

    # -----------------------------------------------------------------
    # 3. It RUNS, and prints exactly what it should.
    # -----------------------------------------------------------------
    local log="$WORK/run-$cc_arch.log"
    timeout 180 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" --timeout 90 \
        "$efi" >"$log" 2>&1
    # Strip CR and the firmware's own chatter before matching.
    tr -d '\r' < "$log" > "$log.clean"

    local line
    for line in "${EXPECTED[@]}"; do
        grep -Fxq "$line" "$log.clean"
        check "$?" "$arch: output line — ${line# *}"
    done

    # -----------------------------------------------------------------
    # 4. A C image pays NOTHING for any of it.
    # -----------------------------------------------------------------
    local cefi="$WORK/hello-$cc_arch.efi"
    "$AXL_CC" --arch "$cc_arch" --release "$C_SRC" -o "$cefi" \
        >"$WORK/cbuild-$cc_arch.log" 2>&1
    rc=$?
    check "$rc" "$arch: the C example still builds (rc=$rc)"
    if [[ "$rc" -eq 0 ]]; then
        n="$(eh_section_count "$cefi" "$objdump")"
        [[ "$n" == "0" ]]
        check "$?" "$arch: the C .efi carries NO unwind sections (found $n)"
    fi

    # And a C++ image that did NOT ask for exceptions pays nothing either --
    # the flag is the switch, not the language.
    local ncefi="$WORK/noeh-$cc_arch.efi"
    "$AXL_CXX" --arch "$cc_arch" --release "$SCRIPT_DIR/cxx-ctor-selftest.cpp" \
        -o "$ncefi" >"$WORK/ncbuild-$cc_arch.log" 2>&1
    rc=$?
    if [[ "$rc" -eq 0 ]]; then
        n="$(eh_section_count "$ncefi" "$objdump")"
        [[ "$n" == "0" ]]
        check "$?" "$arch: a C++ image without -fexceptions carries none either (found $n)"
    else
        check 1 "$arch: the no-exceptions C++ fixture builds (rc=$rc)"
    fi
}

for a in "${ARCHES[@]}"; do
    run_one "$a"
done

# ---------------------------------------------------------------------
# 5. The SHARED linker scripts must not KEEP the unwind sections.
# ---------------------------------------------------------------------
# Arch-independent, so asserted once. This is the source-level form of case 4:
# case 4 catches an image that grew, this catches the edit that would grow
# every image at once, and names the file to fix. The measured cost of getting
# it wrong is +16.8% on a C-only image.
for lds in elf_x86_64_efi.lds elf_aarch64_efi.lds; do
    ! grep -q "KEEP(\*(\.eh_frame))" "$PROJECT_DIR/scripts/$lds"
    check "$?" "scripts/$lds does not KEEP .eh_frame (that belongs in the _eh variant)"
done
# The positive control: the _eh variants MUST keep it, or case 3 would be
# passing for some other reason and this pair would prove nothing.
for lds in elf_x86_64_efi_eh.lds elf_aarch64_efi_eh.lds; do
    grep -q "KEEP(\*(\.eh_frame))" "$PROJECT_DIR/scripts/$lds"
    check "$?" "scripts/$lds DOES keep .eh_frame"
done

echo ""
echo "cxx-exceptions: $pass passed, $fail failed, $skipped arch(es) skipped"
# A run where EVERY arch skipped is not a pass. install.sh stages the glue
# objects silently when they are absent, so a C-only SDK would leave only the
# four source-level grep assertions running and this would report green --
# which is the "gate that cannot see" shape exactly. Same guard, and same exit
# code, as test-cxx-hosted-qemu.sh.
if [[ "$skipped" -eq "${#ARCHES[@]}" ]]; then
    echo "cxx-exceptions: every arch SKIPPED — nothing was actually exercised."
    echo "  Stage the C++ SDK first: scripts/install.sh --arch all --cpp"
    exit 2
fi
[[ "$fail" -eq 0 ]]
