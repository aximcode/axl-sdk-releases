#!/bin/bash
# test-meta: arch=both needs= est=38 local-only=0
# test-cxx-exceptions-qemu.sh — real try/catch under UEFI, and the price a C
# image does NOT pay for it.
#
# `axl-c++ -fexceptions` is the whole opt-in. axl-cc sees the flag (or an input
# object referencing __gxx_personality_v0, so a staged build works too) and
# switches three things at once: the exceptions linker script, which KEEPs
# .eh_frame and defines __eh_frame_start; two extra `objcopy -j` entries, so
# the tables actually reach the image; and the toolchain's libstdc++ /
# libsupc++ / libgcc plus AXL's glue objects. Since P4 that is what EVERY C++
# link takes, so what this suite still pins uniquely is the -fexceptions
# COMPILE half: landing pads in the caller's own frames, and a throw that is
# CAUGHT rather than reaching std::terminate.
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
#   2. THE TERMINATE HANDLER IS AXL'S, and it speaks. libstdc++'s
#      __gnu_cxx::__verbose_terminate_handler prints NOTHING under UEFI -- its
#      output goes to a newlib stderr no UEFI image wires up -- while dragging
#      __cxa_demangle and newlib's stdio into every -fexceptions image for
#      ~112 KB. AXL preempts it with an object on the link line. Asserted two
#      ways, because either alone is weak: the SIZE half (`nm`: the demangler
#      and stdio are absent) would still pass for a handler that printed
#      nothing, and the SPEAKING half (an uncaught throw naming its type and
#      what()) would still pass with 112 KB of dead demangler alongside.
#   3. Both arches. They differ in toolchain provenance (ours vs ARM's) and in
#      relocation handling, and aa64 has diverged there before -- an RTTI link
#      once produced a split DT_RELA the crt0 walked off the end of.
#   4. THE BYTE-IDENTITY CONSTRAINT (AXL-Cxx-Unwinder-Design.md §U2): a C image
#      must be unchanged whether or not the SDK supports exceptions. Measured
#      when that was designed: putting KEEP(*(.eh_frame)) in the SHARED linker
#      script costs a C-only image +11.5-13.6 KB, +16.8%, for tables it can
#      never use. So the C image must carry NO unwind sections, and the shared
#      scripts must contain no KEEP of them. Asserted rather than trusted,
#      because the tempting simplification -- one linker script, KEEP always --
#      is invisible until someone measures an image.
#
#      THE C++ HALF OF THAT INVERTED AT P4, and only the C++ half. A C++ image
#      that did not ask for exceptions now DOES carry the unwind sections,
#      because there is one C++ link shape and it takes the _eh script
#      (AXL-Libc-Substrate-Design.md §4d). The switch is the LANGUAGE, not the
#      flag: libstdc++ is compiled WITH exceptions whatever the caller passed,
#      so `vector::at` out of range really throws in a -fno-exceptions image,
#      and without a registered frame table that throw reaches std::terminate
#      through _URC_FATAL_PHASE1_ERROR -- losing the type name and what() that
#      case 2 above asserts. The C image is still free, which is the half §U2
#      was actually protecting, and `hello.c` is byte-identical across P3 and
#      P4 at 47,247 bytes.
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
TERM_SRC="$SCRIPT_DIR/cxx-terminate-selftest.cpp"
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

# The uncaught-throw fixture, byte for byte — one array per -DTERM_CASE build.
# The type names are MANGLED because preempting libstdc++'s handler is what
# leaves __cxa_demangle out of the image; a demangled "std::runtime_error"
# here would mean the 112 KB came back.
#
# Three cases because the handler has three branches, and the two off the
# golden path are the ones that rot quietly: case 2 is what keeps an uncaught
# NON-std throw diagnosable instead of escaping into libsupc++'s abort, and
# case 3 is the NULL that __cxa_current_exception_type() returns when nothing
# is in flight -- printing a type name there would dereference it.
EXPECTED_TERMINATE_1=(
    "cxx-terminate: throwing a std::runtime_error with no handler"
    "terminate: uncaught exception of type St13runtime_error"
    "  what(): a deliberate uncaught error"
)
EXPECTED_TERMINATE_2=(
    "cxx-terminate: throwing a bare int with no handler"
    "terminate: uncaught exception of type i"
    "  no what(): not derived from std::exception"
)
EXPECTED_TERMINATE_3=(
    "cxx-terminate: calling std::terminate with nothing in flight"
    "terminate: called with no exception in flight"
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

# Defined symbols in the ELF .so axl-cc keeps beside the .efi.
#
# It has to be the .so: objcopy does not carry .symtab into the PE image (see
# scripts/check-no-avx.py, which reaches the same conclusion for the same
# reason), so `nm` on the .efi would report nothing and every ABSENCE assertion
# below would pass while blind. That is the trap eh_section_count above
# documents, so this returns `ERR` the same way -- an unreadable file and an
# empty symbol table are both distinguished from a genuine count of zero.
nm_count() {  # nm_count <so> <nm> <extended-regex> -> count | ERR
    local out
    out="$("$2" --defined-only "$1" 2>/dev/null)" || { echo ERR; return 0; }
    [[ -n "$out" ]]                               || { echo ERR; return 0; }
    grep -cE "$3" <<<"$out"
}

# One -DTERM_CASE build, booted, matched line by line. The case number selects
# the fixture's branch AND names its artefacts, so a failure log is findable.
run_terminate_case() {  # run_terminate_case <arch> <cc_arch> <case> <expected...>
    local arch="$1" cc_arch="$2" case_no="$3"
    shift 3
    local tefi="$WORK/term$case_no-$cc_arch.efi"
    local tlog="$WORK/trun$case_no-$cc_arch.log"
    local rc line

    "$AXL_CXX" --arch "$cc_arch" --release -fexceptions "-DTERM_CASE=$case_no" \
        "$TERM_SRC" -o "$tefi" >"$WORK/tbuild$case_no-$cc_arch.log" 2>&1
    rc=$?
    check "$rc" "$arch: terminate fixture case $case_no builds (rc=$rc)"
    if [[ "$rc" -ne 0 ]]; then
        grep -E "undefined reference|error" "$WORK/tbuild$case_no-$cc_arch.log" \
            | head -5 | sed 's/^/      /'
        return
    fi

    timeout 180 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" --timeout 90 \
        "$tefi" >"$tlog" 2>&1
    tr -d '\r' < "$tlog" > "$tlog.clean"
    for line in "$@"; do
        grep -Fxq "$line" "$tlog.clean"
        check "$?" "$arch: case $case_no output — ${line# *}"
    done
}

run_one() {
    local arch="$1" cc_arch objdump nm rc n
    # The CROSS objdump, from the same manifest every other consumer reads.
    # The host's cannot read pei-aarch64-little at all.
    # shellcheck source=/dev/null
    . "$PROJECT_DIR/scripts/axl-toolchains.conf"
    case "$arch" in
        X64)     cc_arch="x64"
                 objdump="${AXL_X64_BINUTILS_PREFIX:-$AXL_X64_BINUTILS_PREFIX_DEFAULT}objdump"
                 nm="${AXL_X64_BINUTILS_PREFIX:-$AXL_X64_BINUTILS_PREFIX_DEFAULT}nm" ;;
        AARCH64) cc_arch="aa64"
                 objdump="${AXL_AA64_BINUTILS_PREFIX:-$AXL_AA64_BINUTILS_PREFIX_DEFAULT}objdump"
                 nm="${AXL_AA64_BINUTILS_PREFIX:-$AXL_AA64_BINUTILS_PREFIX_DEFAULT}nm" ;;
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
    # 4. The terminate handler is OURS: the size half.
    # -----------------------------------------------------------------
    # __cxa_demangle and newlib's stdio reach an image through exactly one
    # door, libstdc++'s vterminate.o, and preempting that object with ours
    # shuts it. Both are asserted because they are two distinct costs (the
    # demangler is code, stdio drags _impure_ptr's static state with it), and
    # each names its own culprit when it fires.
    #
    # The control is not decoration: without it, a .so that failed to build, or
    # one built WITHOUT exceptions, would satisfy both absences perfectly.
    local so="${efi%.efi}.so"
    n="$(nm_count "$so" "$nm" '__gxx_personality_v0')"
    [[ "$n" != "ERR" && "$n" -ge 1 ]]
    check "$?" "$arch: control — the .so is readable and IS an exceptions link (found $n)"

    n="$(nm_count "$so" "$nm" ' __cxa_demangle$')"
    [[ "$n" == "0" ]]
    check "$?" "$arch: no __cxa_demangle in the image (found $n)"

    n="$(nm_count "$so" "$nm" ' (fputs|fwrite)$')"
    [[ "$n" == "0" ]]
    check "$?" "$arch: no newlib stdio in the image (found $n)"

    # -----------------------------------------------------------------
    # 5. The terminate handler is OURS: the speaking half.
    # -----------------------------------------------------------------
    # Reaching std::terminate must name the exception and its what(). The stock
    # handler writes to a newlib stderr nothing wires up, so it prints NOTHING
    # here -- 112 KB buying negative value. Exact lines, not substrings.
    #
    # One boot per case, because each case ENDS the image. That is the whole
    # cost of covering the two branches off the golden path, and they are worth
    # it: nothing else in the suite reaches them.
    run_terminate_case "$arch" "$cc_arch" 1 "${EXPECTED_TERMINATE_1[@]}"
    run_terminate_case "$arch" "$cc_arch" 2 "${EXPECTED_TERMINATE_2[@]}"
    run_terminate_case "$arch" "$cc_arch" 3 "${EXPECTED_TERMINATE_3[@]}"

    # -----------------------------------------------------------------
    # 6. A C image pays NOTHING for any of it.
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

    # A C++ image that did NOT ask for exceptions carries them ANYWAY, and
    # must: see note 4 in the header. INVERTED AT P4 -- this used to assert
    # zero, on the reasoning that the flag was the switch. The language is the
    # switch now, because libstdc++ throws whatever the caller compiled with.
    #
    # `> 0` rather than `== 2`: which of .eh_frame / .gcc_except_table a given
    # fixture needs depends on whether it has landing pads, and pinning the
    # exact pair would fail for a reason that is not a defect. ERR is excluded
    # explicitly -- it is the unreadable-image case the helper exists to keep
    # distinct from a real zero, and `[[ ERR -gt 0 ]]` would be a bash arith
    # error rather than a clean fail.
    local ncefi="$WORK/noeh-$cc_arch.efi"
    "$AXL_CXX" --arch "$cc_arch" --release "$SCRIPT_DIR/cxx-ctor-selftest.cpp" \
        -o "$ncefi" >"$WORK/ncbuild-$cc_arch.log" 2>&1
    rc=$?
    if [[ "$rc" -eq 0 ]]; then
        n="$(eh_section_count "$ncefi" "$objdump")"
        [[ "$n" != "ERR" && "$n" -gt 0 ]]
        check "$?" "$arch: a C++ image WITHOUT -fexceptions still carries them (found $n)"
    else
        check 1 "$arch: the no-exceptions C++ fixture builds (rc=$rc)"
    fi
}

for a in "${ARCHES[@]}"; do
    run_one "$a"
done

# ---------------------------------------------------------------------
# 7. The SHARED linker scripts must not KEEP the unwind sections.
# ---------------------------------------------------------------------
# Arch-independent, so asserted once. This is the source-level form of case 6:
# case 6 catches an image that grew, this catches the edit that would grow
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
