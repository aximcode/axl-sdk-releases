#!/bin/bash
# test-meta: arch=both needs= est=13 local-only=0
# test-cxx-noeh-qemu.sh — `axl-c++ --no-eh-frame`: an ~18% smaller C++ image
# that still halts readably.
#
# WHAT THE FLAG IS FOR. Since P4 every C++ link takes the exceptions linker
# script, whose `.eh_frame` KEEP is ~11-18% of the image. A consumer that uses
# neither exceptions nor iostreams pays for a frame table it cannot use.
#
# WHY IT IS NOT JUST A SCRIPT SWAP, which is what makes this test necessary:
#
#   1. Dropping the KEEP alone does not LINK. axl-cxxrt-eh.o references
#      __eh_frame_start, which only the exceptions script defines, and every
#      build path passes --no-undefined. The flag swaps that object for
#      axl-cxxrt-nothrow.o.
#   2. Dropping the frame table alone does not DEGRADE, it CRASHES. Measured
#      on this tree: vector::at(99) in such an image takes an unhandled CPU
#      fault and WEDGES the machine -- the firmware never regains control. An
#      empty-but-valid .eh_frame was tried and faults identically, so the
#      interception has to happen before the unwinder is entered.
#      axl-cxxrt-nothrow.o's __cxa_throw is that interception, and case 2
#      below is what proves the wedge is gone.
#   3. Combining it with -fexceptions is a CORRECTNESS failure, not a size
#      trade: no catch block can run, so handlers are silently dead. axl-cc
#      REFUSES the combination, and cases 4 and 5 pin both detection paths --
#      the flag on the command line, and a pre-built object that was compiled
#      with it (the staged -c-then-link build, which reaches the link with no
#      source to inspect).
#
# Auxiliary single-binary test (opts out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-noeh-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

AXL_CXX="$(test_sdk_dir)/bin/axl-c++"
THROW_SRC="$PROJECT_DIR/test/integration/cxx-hosted-throw.cpp"
PLAIN_SRC="$PROJECT_DIR/sdk/examples/containers.cpp"
LIB_DIR="$(test_sdk_dir)/lib/axl/$_native_arch"

WORK="$TEST_TMPDIR/cxx-noeh"
mkdir -p "$WORK"

pass=0
fail=0
check() {  # check <msg> <cmd...>
    local msg="$1"; shift
    if "$@"; then
        echo "  PASS: $msg"; pass=$((pass + 1))
    else
        echo "  FAIL: $msg"; fail=$((fail + 1))
    fi
}
check_absent() {  # check_absent <msg> <pattern> <file>
    local msg="$1" pat="$2" file="$3"
    if grep -aqF "$pat" "$file"; then
        echo "  FAIL: $msg"; fail=$((fail + 1))
        grep -aF "$pat" "$file" | sed 's/^/      /' | head -3
    else
        echo "  PASS: $msg"; pass=$((pass + 1))
    fi
}

echo "=== C++ --no-eh-frame Test ($TEST_ARCH) ==="

if [[ ! -x "$AXL_CXX" || ! -f "$LIB_DIR/axl-cxxrt-nothrow.o" ]]; then
    echo "WARN: no staged C++ SDK with axl-cxxrt-nothrow.o at $LIB_DIR."
    echo "      run: scripts/install.sh --arch all --cpp"
    echo "C++ --no-eh-frame test: SKIP"
    exit 0
fi

# ---------------------------------------------------------------------------
# Build both shapes of the throwing fixture, and the non-throwing control.
# ---------------------------------------------------------------------------
check "default build (exceptions script) succeeds" \
    "$AXL_CXX" --arch "$_native_arch" --release "$THROW_SRC" -o "$WORK/throw-eh.efi"
check "--no-eh-frame build succeeds" \
    "$AXL_CXX" --arch "$_native_arch" --release --no-eh-frame "$THROW_SRC" \
        -o "$WORK/throw-noeh.efi"
check "--no-eh-frame builds a NON-throwing image too" \
    "$AXL_CXX" --arch "$_native_arch" --release --no-eh-frame "$PLAIN_SRC" \
        -o "$WORK/plain-noeh.efi"

# --- 1. the size win, which is the entire point of the flag ---------------
if [[ -f "$WORK/throw-eh.efi" && -f "$WORK/throw-noeh.efi" ]]; then
    _eh_sz=$(stat -c%s "$WORK/throw-eh.efi")
    _no_sz=$(stat -c%s "$WORK/throw-noeh.efi")
    _pct=$(( (_eh_sz - _no_sz) * 100 / _eh_sz ))
    echo "  sizes: with-eh $_eh_sz, --no-eh-frame $_no_sz (-${_pct}%)"
    # A FLOOR, not an exact number: the saving tracks how much of libstdc++'s
    # throw path --gc-sections can reach, which moves with the toolchain. 10%
    # is well under the ~18% measured and still fails loudly if the flag
    # silently stops changing the link.
    check "--no-eh-frame saves at least 10% of the image (got ${_pct}%)" \
        test "$_pct" -ge 10
else
    check "SKIP balancer: a build failed, so no size comparison" false
fi

# --- 4 + 5. the guard, both detection paths -------------------------------
# Refusal is asserted on the EXIT CODE and on no output file, not on the
# message: a guard that printed a complaint and built the image anyway would
# satisfy a message-only check, and that is exactly the bug this had before
# the guard was moved after $CROSS was resolved.
rm -f "$WORK/guard1.efi"
if "$AXL_CXX" --arch "$_native_arch" --release --no-eh-frame -fexceptions \
        "$THROW_SRC" -o "$WORK/guard1.efi" >"$WORK/guard1.log" 2>&1; then
    echo "  FAIL: --no-eh-frame with -fexceptions was ACCEPTED"; fail=$((fail + 1))
else
    echo "  PASS: --no-eh-frame with -fexceptions is refused"; pass=$((pass + 1))
fi
check "the refused build produced no image" test ! -f "$WORK/guard1.efi"
check "the refusal says why" grep -qF "cannot be combined with exceptions" "$WORK/guard1.log"

# The staged path: an object compiled -fexceptions, linked later with no
# source present. Detected through __gxx_personality_v0, which is the only
# evidence left at that point.
rm -f "$WORK/guard2.efi"
if "$AXL_CXX" --arch "$_native_arch" --release -fexceptions -c "$THROW_SRC" \
        -o "$WORK/staged.o" >/dev/null 2>&1; then
    if "$AXL_CXX" --arch "$_native_arch" --release --no-eh-frame "$WORK/staged.o" \
            -o "$WORK/guard2.efi" >"$WORK/guard2.log" 2>&1; then
        echo "  FAIL: --no-eh-frame over an -fexceptions OBJECT was ACCEPTED"
        fail=$((fail + 1))
    else
        echo "  PASS: --no-eh-frame over an -fexceptions OBJECT is refused"
        pass=$((pass + 1))
    fi
    check "the object refusal names the object" \
        grep -qF "__gxx_personality_v0" "$WORK/guard2.log"
else
    check "SKIP balancer: -fexceptions -c did not build" false
    check "SKIP balancer: so the object guard was not exercised" false
fi

# ---------------------------------------------------------------------------
# Run all three images. The throwing pair is the assertion that matters.
# ---------------------------------------------------------------------------
test_add_efi "$WORK/throw-eh.efi"
test_add_efi "$WORK/throw-noeh.efi"
test_add_efi "$WORK/plain-noeh.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo NOEH_BEGIN"
    echo "echo --CASE-PLAIN--"
    echo "plain-noeh.efi"
    echo "echo --CASE-EH--"
    echo "throw-eh.efi"
    echo "echo --CASE-NOEH--"
    echo "throw-noeh.efi"
    echo "echo NOEH_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 150
test_clean_log

echo "--- serial log (NOEH_BEGIN .. NOEH_DONE) ---"
sed -n '/NOEH_BEGIN/,/NOEH_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'
echo "--- assertions ---"

# --- 3. a non-throwing image is BEHAVIOURALLY IDENTICAL -------------------
# The flag must be invisible on every path that does not throw, which is the
# path a consumer adopting it actually runs.
for line in "sorted: apple fig pear" "fig is 3 chars, 3 entries"; do
    check "plain --no-eh-frame: $line" grep -aFxq "$line" "$TEST_CLEAN_LOG"
done

# --- 2. the throw HALTS READABLY, not with a wedge ------------------------
check "throw reached the call (both images)" \
    grep -aFxq "cxx-throw: about to call vector::at(99) on a 3-element vector" \
        "$TEST_CLEAN_LOG"

# The default build's diagnostic, unchanged by this work.
check "with-eh: names the type" \
    grep -aFxq "terminate: uncaught exception of type St12out_of_range" "$TEST_CLEAN_LOG"

# The --no-eh-frame build's. Distinct text on purpose: an image that cannot
# unwind should SAY so, and the difference is what tells the two runs apart in
# one log. The what() line is the part that would be lost by a naive opt-out.
check "--no-eh-frame: names the type AND says why the message differs" \
    grep -aFxq "terminate: throw of type St12out_of_range (image linked --no-eh-frame)" \
        "$TEST_CLEAN_LOG"
check "--no-eh-frame: recovers what() with no unwinder" \
    grep -aFxq "  what(): vector::_M_range_check: __n (which is 99) >= this->size() (which is 3)" \
        "$TEST_CLEAN_LOG"

# Neither image may RETURN from the throw.
check_absent "no image returned from the throw" \
    "cxx-throw: UNREACHABLE" "$TEST_CLEAN_LOG"
# The positive control for that absence: the string must still be in the
# fixture, or the check silently stops watching.
check "the UNREACHABLE probe still exists in the fixture" \
    grep -qF "cxx-throw: UNREACHABLE" "$THROW_SRC"

# The wedge this flag exists to avoid. A faulting image dumps CPU registers
# and never reaches the next shell command, so BOTH are asserted.
check_absent "no CPU fault dump (the wedge a naive opt-out produces)" \
    "!!!! Find image based on IP" "$TEST_CLEAN_LOG"
check "the shell survived every case" grep -aq '^NOEH_DONE' "$TEST_CLEAN_LOG"

echo ""
echo "cxx-noeh ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
