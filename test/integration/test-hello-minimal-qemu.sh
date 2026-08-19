#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0
# test-hello-minimal-qemu.sh — sdk/examples/hello-minimal.{c,cpp} boot and echo
# their argument, in both languages.
#
# WHAT THIS PROTECTS. hello-minimal links NO libaxl: it hand-declares the four
# firmware structures it touches and is entered through AXL's assembly CRT0
# only. That buys ~4.6 KB against ~47 KB for the smallest image that does link
# libaxl -- and it makes the example uniquely fragile in ways a compile gate
# cannot see. All three of these were REAL failures while it was written, and
# each one is silent or misleading at the source level:
#
#   1. Entry contract. Pointing `ld -e` straight at C is rejected by the shell
#      ("Script Error Status: Invalid Parameter"): the image still needs the
#      asm CRT0 + axl-reloc.o for .bss clear and DT_RELA walking.
#   2. Calling convention. UEFI is the MS x64 ABI. Without ms_abi on the entry
#      AND on every firmware function pointer, it faults with a bare #GP and no
#      symbols -- while the struct offsets are already correct. AArch64 has one
#      convention, so an aa64-only check would pass and x64 would break.
#   3. Struct offsets. A wrong one is not reliably a crash: the C build faulted
#      and the C++ build HUNG on `call *0x140(%rax)` (BootServices+320 rather
#      than +152).
#
# None of that is visible to check-examples, which only compiles. Booting the
# image and reading back the argument is the only check that covers it, and it
# must run on BOTH arches for reason 2.
#
# `check-examples` still carries the compile half, and the Makefile rule
# asserts .cpp registers no .init_array entry -- there is no runtime here to
# walk it, so a global constructor would be silently unconstructed.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-hello-minimal-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    hello-minimal 2>&1 | tail -2

test_add_efi "$NATIVE_DIR/hello-minimal-c.efi"   "app/hmin-c.efi"
test_add_efi "$NATIVE_DIR/hello-minimal-cxx.efi" "app/hmin-cxx.efi"

# Distinct arguments per language, so a test that somehow ran one image twice
# cannot look like both passing.
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "app\\hmin-c.efi ARG-FROM-C"
    echo "app\\hmin-cxx.efi ARG-FROM-CXX"
    echo "echo HMIN_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== hello-minimal: no-libaxl images boot and read argv ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

if ! test_wait_for "HMIN_DONE" 120; then
    echo "FAIL: fixture did not finish within 120s"
    test_clean_log; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

# EXACT whole lines. The failure modes above produce a fault, a hang or NO
# output -- but a half-working argument parser would produce "hello, " with a
# truncated or empty argument, and a substring match on "hello" would wave that
# through. The argument is the assertion.
expect_line() {
    local want="$1" what="$2"
    if [[ "$(grep -c -F -x "$want" "$TEST_CLEAN_LOG" 2>/dev/null || true)" == "1" ]]; then
        pass "$what"
    else
        fail "$what — expected exactly one '$want'"
    fi
}

expect_line "hello, ARG-FROM-C"   "C image boots and echoes its argument"
expect_line "hello, ARG-FROM-CXX" "C++ image boots and echoes its argument"

# A CPU exception prints a register dump rather than nothing, so name it
# explicitly: it is the signature of the calling-convention and offset bugs,
# and saying so turns a future failure into a diagnosis.
if grep -qiE 'Exception Type|Synchronous Exception' "$TEST_CLEAN_LOG"; then
    fail "a CPU exception was raised — check ms_abi on the entry and on every
        firmware function pointer, then the struct offsets"
    grep -iE 'Exception Type|Synchronous Exception' "$TEST_CLEAN_LOG" | head -2 | sed 's/^/      /'
else
    pass "no CPU exception"
fi

# The size claim is the whole reason this example exists, so assert it rather
# than leaving it to prose that can rot. 12 KB is loose on purpose: it is a
# regression bound, not a target, and it still catches "someone linked libaxl"
# (which lands at ~47 KB).
for v in c cxx; do
    sz=$(stat -c%s "$NATIVE_DIR/hello-minimal-$v.efi")
    if [[ "$sz" -lt 12288 ]]; then
        pass "hello-minimal-$v.efi stays tiny ($sz bytes)"
    else
        fail "hello-minimal-$v.efi is $sz bytes — it is meant to link no libaxl"
    fi
done

echo ""
printf "hello-minimal: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 5 ]] && exit 0 || exit 1
