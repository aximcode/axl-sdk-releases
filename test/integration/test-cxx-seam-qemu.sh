#!/bin/bash
# test-meta: arch=both needs= est=75 local-only=0
# test-cxx-seam-qemu.sh — the C++ seams over AXL's C API, run under QEMU.
#
# Covers AXL-Cxx-Design.md §9 phases C2, C3 and C5:
#
#   C2  axl::view / axl::adopt                     <axl/axl-cstr.hpp>
#   C3  axl::array_span / axl::array_ptr_span      <axl/axl-array.hpp>
#   C5  the AxlNTree ranges                        <axl/axl-ntree.hpp>
#       axl::radix_tree<T>                         <axl/axl-radix-tree.hpp>
#       axl::gfx_target_scope                      <axl/axl-gfx-surface.hpp>
#
# What this script pins that the fixture alone cannot:
#
#   1. Exact output lines, via grep -Fxq. A substring match would let a
#      traversal that emits "abcdef" satisfy an assertion about "abc".
#   2. The MISMATCH verb HALTS. axl::array_span<T> over an array whose stride
#      is not sizeof(T) is an out-of-bounds read when sizeof(T) is the larger,
#      so it aborts with both sizes named. Two assertions, because they fail
#      independently: the diagnostic must appear, AND the line after the call
#      must NOT — a check that printed and then carried on would satisfy the
#      first on its own.
#   3. No leaks, with a positive control. The fixture allocates AxlArrays, an
#      AxlNTree, two AxlGfxBuffers, two radix trees and (through axl::adopt)
#      several axl_strdup'd strings. A run where AXL_MEM_DEBUG was off would
#      report no leaks because nothing was watching, so the count of clean
#      verdicts is asserted too.
#   4. The STAGED SDK builds the same source through axl-c++. install.sh
#      copies include/axl/*.hpp wholesale, so a new header arrives without a
#      packaging change — that is the claim, and this is what checks it.
#
# Auxiliary single-binary test (opts out of the test-axl.sh ratchet).
# Requires a staged SDK for the consumer-toolchain case:
#   scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-seam-qemu.sh [--arch X64|AARCH64]

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
SRC="$PROJECT_DIR/test/integration/cxx-seam-selftest.cpp"
LIB_DIR="$(test_sdk_dir)/lib/axl/$_native_arch"
NATIVE_DIR="$(test_build_dir)"

WORK="$TEST_TMPDIR/cxx-seam"
mkdir -p "$WORK"

pass=0
fail=0
# Takes a COMMAND, not an exit code: common-test.sh sets `-e`, so a bare
# `[[ ... ]]; check "$?"` aborts the whole script the first time it is false.
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

echo "=== C++ Seam Test ($TEST_ARCH) ==="

# ---------------------------------------------------------------------------
# The image that RUNS is the Makefile's, built DEBUG so AXL_MEM_DEBUG is on.
# scripts/install.sh stages RELEASE.
#
# NOT for the `cstr: freed` assertion, which an earlier revision of this
# comment claimed: axl_mem_get_stats' live count is maintained OUTSIDE
# #ifdef AXL_MEM_DEBUG (src/mem/axl-mem.c), so that one works in RELEASE too.
# It is for the TEARDOWN LEAK REPORT -- _axl_mem_dump_leaks_at_exit is
# debug-only, and the leak gate at the bottom is the assertion that needs it.
# ---------------------------------------------------------------------------
echo "--- building the DEBUG fixture (AXL_MEM_DEBUG on) ---"
make -C "$PROJECT_DIR" ARCH="$_native_arch" AXL_CPP=1 \
    ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} cxx-seam-selftest 2>&1 | tail -3

EFI="$NATIVE_DIR/cxx-seam-selftest.efi"
if [[ ! -f "$EFI" ]]; then
    echo "WARN: cxx-seam-selftest.efi not built on this box; skipping."
    echo "C++ seam test: SKIP"
    exit 0
fi
test_add_efi "$EFI"

# ---------------------------------------------------------------------------
# The CONSUMER toolchain must build the same source from the staged headers.
# This is what proves axl-c++ and install.sh carry the five new .hpp files;
# it is a compile+link assertion, not the image that runs.
# ---------------------------------------------------------------------------
if [[ -x "$AXL_CXX" && -f "$LIB_DIR/axl-cxxrt-terminate.o" ]]; then
    # The staged SDK is a SECOND TREE: axl-c++ compiles against the staged
    # include dir, so an un-restaged edit to include/axl means this exercises
    # the PREVIOUS build. Compared by content -- install.sh deliberately
    # avoids mtime churn, so an mtime test reports false drift.
    check "staged headers match include/axl (else: install.sh --arch all --cpp)" \
        diff -rq "$PROJECT_DIR/include/axl" "$(test_sdk_dir)/include/axl-sdk/axl"

    check "axl-c++ builds the fixture from the staged SDK" \
        "$AXL_CXX" --arch "$_native_arch" --release "$SRC" -o "$WORK/consumer.efi"
else
    echo "  NOTE: no staged C++ SDK at $LIB_DIR;"
    echo "        skipping the consumer-toolchain assertions only."
    echo "        run: scripts/install.sh --arch all --cpp"
    # BALANCED SKIP (feedback_balancer_count): the populated branch above adds
    # two assertions, so this one must add two as well. Without them the run
    # silently reports 2 fewer passes and still exits 0, and the total below
    # could not be pinned to a constant.
    check "SKIP balancer: staged SDK absent, so no header-drift check" true
    check "SKIP balancer: staged SDK absent, so no consumer-toolchain build" true
fi

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo SEAM_BEGIN"
    echo "cxx-seam-selftest.efi cstr"
    echo "cxx-seam-selftest.efi array"
    echo "cxx-seam-selftest.efi ntree"
    echo "cxx-seam-selftest.efi radix"
    echo "cxx-seam-selftest.efi gfx"
    # Runs LAST: it halts, and a halt that took the shell down with it would
    # otherwise cost every verb after it.
    echo "echo SEAM_MISMATCH"
    echo "cxx-seam-selftest.efi mismatch"
    echo "echo SEAM_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 120
test_clean_log

# ---------------------------------------------------------------------------
# Expectations. Every one is an EXACT line (grep -Fxq).
# ---------------------------------------------------------------------------
EXPECTED=(
    # --- C2: axl::view / axl::adopt --------------------------------------
    "cstr: view 5 1"
    # size 0, empty, data() non-NULL, and data()[0] == '\0'. The last two are
    # what separate this from a default-constructed string_view.
    "cstr: viewnull 0 1 1 1"
    # data() checked on the NON-NULL empty input too -- asserting it only on
    # the NULL input let view("") return a null-data view and stay green.
    "cstr: viewempty 0 1 1 1"
    "cstr: adopt adopted 7"
    "cstr: adoptbig 51 1"
    "cstr: adoptnull 1 0"
    # THE C2 assertion: the live axl_malloc count is back to its baseline, so
    # every adopted buffer was released. "adopt returned the right string"
    # passes just as well while leaking.
    "cstr: freed 1"
    "cstr: typed typed-target 12 0"
    "cstr: typednull 1 0"
    # Past axl::string's 23-byte inline buffer, so this is the heap path; the
    # two cases above are short enough that bad() was false BY CONSTRUCTION.
    "cstr: typedbig 42 0 1"
    # THE reason adopt<> is templated: std::string halts on a failed copy,
    # axl::string sets bad(). Without this the one documented difference
    # between the two destinations has no test.
    "cstr: typedoom 1 1"
    "cstr: done"

    # --- C3: the spans ---------------------------------------------------
    "array: span 5 5 1"
    "array: sum 15"
    # std::sort wrote THROUGH the span into the array's own buffer; these
    # three values are read back with axl_array_get, not from the span.
    "array: sorted 1 3 5"
    "array: evens 6"
    "array: ro 5 1"
    "array: null 0 1"
    "array: ptrspan 3 10 30"
    "array: ptrfilter 40"
    "array: find 1"
    "array: ptrnull 0"
    "array: constptr 3 20"
    # A non-NULL, ZERO-LENGTH array: the only case separating "empty span over
    # a real buffer" from "span over nothing".
    "array: emptyarr 0 1"
    "array: constempty 0"
    "array: stolen 0 1"
    "array: nulllit 0 0"
    "array: done"

    # --- C5: the AxlNTree ranges -----------------------------------------
    "ntree: children bcf 3"
    "ntree: leaf 0 0"
    "ntree: ancestors ca 2 1"
    "ntree: rootup 0"
    "ntree: preorder abcdef 1"
    "ntree: postorder bdecfa 6"
    # Starting at c must not escape to c's sibling f.
    "ntree: subtree cde"
    "ntree: pruned abcf a abcdef"
    "ntree: subpost dec"
    "ntree: single 1 1"
    "ntree: nullwalk 0 0"
    "ntree: filtered def"
    "ntree: constchildren bcf"
    "ntree: dataof 1 1"
    # The const overloads: members of a class template are instantiated only
    # on use, so these three had never been COMPILED before this line existed.
    "ntree: constancestors ca"
    "ntree: constpre cde"
    "ntree: constpost dec"
    # Post-increment: nothing in a range-for, ranges::distance or views::filter
    # uses it, so `return *this` instead of the saved copy shipped silently.
    "ntree: postinc b c a b"
    "ntree: nullfactories 0 0 0 0"
    "ntree: nodata 1"
    # enable_borrowed_range: without it this line does not COMPILE, because
    # ranges::find_if over a temporary range yields std::ranges::dangling.
    "ntree: borrowed c"
    "ntree: done"

    # --- C5: axl::radix_tree ---------------------------------------------
    "radix: valid 1 1 0"
    "radix: size 3 0"
    "radix: lookup 2 1"
    "radix: prefix 1 users/7 1"
    "radix: prefixnosuffix 1"
    "radix: nomatch 1"
    "radix: foreach 3 106"
    "radix: remove 1 0 2"
    # The moved-from tree is invalid and empty; the destination carries the
    # entries. Both halves matter -- a move that copied the handle without
    # clearing the source would double-free at scope exit.
    "radix: moved 1 2 0 0"
    "radix: movedlookup 1"
    "radix: emptyops 1 0 0"
    # A plain function NAME, which did not compile before the for_each shim.
    "radix: fnvisitor 5 0"
    "radix: accessors 1 1"
    "radix: boolinvalid 0"
    # Move assignment, release, get and operator bool had never been
    # instantiated at all -- a wrong body would not even have been compiled.
    "radix: moveassign 1 2 0"
    "radix: selfmove 1 2"
    "radix: release 1 0 2"
    "radix: suffixkept 1 1"
    # A NULL value is a real entry: counted once (not twice), visited by
    # for_each, and removable. All three were broken before the C-side fix.
    "radix: nullvalue 1 0 1"
    "radix: nullgone 0"
    "radix: ownremove 1 1"
    # Replacing a key frees the old value; the leak gate is the other half.
    "radix: ownreplace 2 1"
    "radix: owned 3"
    "radix: done"

    # --- C5: axl::gfx_target_scope ---------------------------------------
    "gfx: buffers 1"
    "gfx: base 1"
    "gfx: outer 1 1"
    "gfx: inner 1 1"
    # THE C5 assertion: the inner scope restored OUTER, not NULL. NULL is the
    # SCREEN, not "no target", so a guard that reset to it would silently
    # redirect the rest of the outer scope's drawing to the display.
    "gfx: restored 1"
    "gfx: unwound 1"
    "gfx: toscreen 1 1"
    "gfx: backtobuf 1"
    "gfx: done"

    # --- C3: the halt ----------------------------------------------------
    "mismatch: about to read an int64_t array as a 40-byte struct"
    "axl::array_span: element size 40 does not match the array's 8"
)

echo "--- serial log (SEAM_BEGIN .. SEAM_DONE) ---"
sed -n '/SEAM_BEGIN/,/SEAM_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'
echo "--- assertions ---"

# -a on every grep: a stray NUL in the serial capture makes GNU grep treat the
# whole file as binary, which silently truncates listings while leaving counts
# correct. AARCH64 emits one reliably.
for line in "${EXPECTED[@]}"; do
    check "$line" grep -aFxq "$line" "$TEST_CLEAN_LOG"
done

# The halt must not RETURN. Separate from the diagnostic assertion above
# because they fail independently: a check that printed its complaint and then
# handed back a span would satisfy that one and this is what catches it.
check_absent "the mismatch did not return (no UNREACHABLE line)" \
    'mismatch: UNREACHABLE' "$TEST_CLEAN_LOG"

# The positive control for the assertion above. An absence check is only worth
# anything if the string could have appeared -- edit the fixture's printf and
# the check silently stops watching, forever. Pinning the pattern against the
# SOURCE is what keeps the two from drifting apart.
check "the UNREACHABLE probe still exists in the fixture" \
    grep -qF 'mismatch: UNREACHABLE' "$SRC"

check "startup.nsh reached the mismatch verb" \
    grep -aq '^SEAM_MISMATCH' "$TEST_CLEAN_LOG"
check "startup.nsh ran every verb to completion" \
    grep -aq '^SEAM_DONE' "$TEST_CLEAN_LOG"

# ---------------------------------------------------------------------------
# Leaks. The positive control matters as much as the check: with AXL_MEM_DEBUG
# off there would be no accounting, and "no leak report" would mean "nothing
# was watching" rather than "nothing leaked".
#
# EXACTLY six, not "at least". Six verbs run and ALL SIX print a verdict --
# including `mismatch`, which halts: abort() reaches newlib's _exit, which is
# AXL's (src/cxxrt/axl-cxxrt-stubs.c), which calls axl_exit -> _axl_cleanup,
# and that drains atexit and THEN dumps leaks. Measured, after an earlier
# revision of this comment asserted the opposite and set the floor to 5.
#
# A `-ge` here would let any single verb lose its verdict entirely -- a fault
# before teardown, an early boot_exit, a silenced console -- and stay green,
# which is the failure this gate exists to prevent rather than exhibit.
# ---------------------------------------------------------------------------
n_leak=$(grep -acF '=== AxlMem leak report:' "$TEST_CLEAN_LOG" || true)
n_ok=$(grep -acF 'mem: no leaks detected' "$TEST_CLEAN_LOG" || true)

check "no leak report (found $n_leak)" test "$n_leak" -eq 0
check "leak accounting was ON: $n_ok clean verdicts (need exactly 6)" \
    test "$n_ok" -eq 6

# The RUN'S OWN RATCHET. TEST_SKIP_RATCHET=1 opts this script out of the
# suite-wide count check, so without this line deleting an entry from EXPECTED
# is invisible -- the run reports a smaller number and still exits 0. The SKIP
# balancers above are what make the constant hold on a box with no staged SDK.
# Counts ITSELF (the + 1), so this constant equals the total printed below
# rather than being one less than it for a reason nobody would remember.
EXPECT_TOTAL=94
check "assertion count is $EXPECT_TOTAL (else EXPECTED lost an entry)" \
    test $((pass + fail + 1)) -eq "$EXPECT_TOTAL"

echo ""
echo "cxx-seam ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
