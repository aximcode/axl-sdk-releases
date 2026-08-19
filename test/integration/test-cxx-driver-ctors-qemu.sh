#!/bin/bash
# test-meta: arch=both needs= est=22 local-only=0
# test-cxx-driver-ctors-qemu.sh — a C++ DRIVER image runs its global
# constructors, and runs the matching destructors on unload.
#
# THE DEFECT THIS GUARDS. An app image initialised the C++ runtime and a driver
# image did not:
#
#   app     AXL_APP    -> _axl_init()       -> _axl_cxxabi_run_init_array()
#   driver  AXL_DRIVER -> axl_driver_init()    <-- never got there
#
# So a driver image had __init_array_start / __init_array_end bracketing real
# constructors and NO code that walked them. Measured on the fixtures this test
# builds, before the fix: 2 constructors registered, 0 walkers, in all three
# images. No link error, no warning, no runtime diagnostic — every global with
# a non-trivial constructor was simply unconstructed.
#
# WHY IT NEEDED A RUNTIME TEST AND NOT JUST A LINK CHECK. crt0 zeroes .bss, so
# an unconstructed global reads all-zero rather than as garbage: the symptom is
# a plausible wrong value far from the cause. That is the same shape as the
# bss-not-zeroed crt0 regression (f7886537), which cost a session precisely
# because the symptom was nowhere near the cause.
#
#   RED    CTORDRV: entry magic=0x00000000 runs=0 bss=0
#   GREEN  CTORDRV: entry magic=0x5a5ac0de runs=1 bss=0
#
# `bss=0` is not padding. It is a `volatile` word nothing ever writes, so it
# proves .bss really was zeroed — without it, `magic=0` would be
# indistinguishable from a magic value that happens to be 0, and the assertion
# would be a coin flip. The `volatile` is load-bearing and was missing at first:
# a plain static is eliminated outright (measured: symbol absent, the argument
# compiles to `mov $0x0,%r8d`), which made this field a tautology dressed up as
# the key evidence.
#
# `runs=1` on BOTH of the first two cycles is the RELOAD assertion: the second
# load of the same image must construct again against freshly zeroed storage.
# A driver, unlike an app, can be unloaded and reloaded within one boot.
#
# ALL THREE DriverEntry-emitting macros are covered, because each is a
# different route to axl_driver_init and only one of them was ever exercised
# by a consumer: AXL_DRIVER directly, AXL_SHARED_DRIVER through its expansion,
# AXL_SERVICE_DRIVER through _axl_service_driver_init.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-cxx-driver-ctors-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    AXL_CPP=1 cxx-ctor-test 2>&1 | tail -2

test_add_efi "$NATIVE_DIR/cxx-ctor-test.efi"       "app/cxx-ctor-test.efi"
test_add_efi "$NATIVE_DIR/cxx-ctor-driver.efi"     "app/cxx-ctor-driver.efi"
test_add_efi "$NATIVE_DIR/cxx-ctor-sd-driver.efi"  "app/cxx-ctor-sd-driver.efi"
test_add_efi "$NATIVE_DIR/cxx-ctor-svc-driver.efi" "app/cxx-ctor-svc-driver.efi"
test_add_efi "$NATIVE_DIR/cxx-ctor-fe-driver.efi"  "app/cxx-ctor-fe-driver.efi"
test_add_efi "$NATIVE_DIR/cxx-ctor-fu-driver.efi"  "app/cxx-ctor-fu-driver.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "app\\cxx-ctor-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== C++ driver global constructors ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

if ! test_wait_for "CTOR_DONE" 120; then
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

# EXACT whole lines, counted. `grep -c -F -x` and not a substring match: the
# RED line differs from the GREEN one only in the digits, so a substring test
# on "CTORDRV: entry" would pass against an unconstructed global — which is
# precisely the regression this file exists to catch.
count_line() { grep -c -F -x "$1" "$TEST_CLEAN_LOG" 2>/dev/null || true; }

expect_count() {
    local want="$1" line="$2" what="$3"
    local got; got="$(count_line "$line")"
    if [[ "$got" == "$want" ]]; then
        pass "$what"
    else
        fail "$what — expected $want of '$line', saw $got"
    fi
}

# 1-2. AXL_DRIVER: constructed on the first load AND on the reload. One
#      assertion covers both because the count is 2 and each line carries
#      runs=1 — a reload that reused dirty .bss would print runs=2 and a
#      constructor that never ran would print magic=0x00000000.
expect_count 2 "CTORDRV: entry magic=0x5a5ac0de runs=1 bss=0" \
    "AXL_DRIVER constructs on both the initial load and the reload"
expect_count 2 "CTORDRV: DTOR" \
    "AXL_DRIVER runs global destructors on both unloads"

# 3. Ordering: the consumer's unload runs BEFORE global destructors, mirroring
#    an app's main-returns-then-_axl_cleanup order. A consumer's unload may
#    still touch globals, so the reverse would hand it destructed state.
#
#    BOTH cycles, not just the first — `awk NR<=4` over the interleaved pair
#    rather than `head -2`, which examined cycle 1 alone and is SIGPIPE-fragile
#    once the match set outgrows grep's stdio buffer. `-F` on the comparison so
#    that a future `-E` cannot silently reinterpret the `|` separators as an
#    alternation matching almost anything.
_ord="$(grep -F -x -e "CTORDRV: unload" -e "CTORDRV: DTOR" "$TEST_CLEAN_LOG" \
        | awk 'NR<=4' | tr '\n' '|')"
if [[ "$_ord" == "CTORDRV: unload|CTORDRV: DTOR|CTORDRV: unload|CTORDRV: DTOR|" ]]; then
    pass "consumer unload runs before global destructors (both cycles)"
else
    fail "unload/destructor ordering — saw '$_ord'"
fi

# 4-6. AXL_SHARED_DRIVER — reaches axl_driver_init through its AXL_DRIVER
#      expansion. init_fn runs inside DriverEntry, before the vtable publish;
#      run is then reached through a real locate+dispatch from the launcher,
#      which is the only way to observe a global from a DISPATCH rather than
#      from the load. That is the pattern's whole promise: the image stays
#      resident and `run` executes against state built once, at load. The
#      identical runs=1 on both lines is what says the constructor did not
#      re-run per dispatch.
expect_count 1 "CTORSD: init magic=0x5a5ac0de runs=1 bss=0" \
    "AXL_SHARED_DRIVER constructs before init_fn"
expect_count 1 "CTORSD: run magic=0x5a5ac0de runs=1 bss=0" \
    "AXL_SHARED_DRIVER globals survive load -> dispatch, constructed once"
expect_count 1 "CTORSD: DTOR" \
    "AXL_SHARED_DRIVER runs global destructors on unload"

expect_ordered() {
    local what="$1"; shift
    local want="" got
    for _l in "$@"; do want="$want$_l|"; done
    got="$(grep -F -x "${@/#/-e}" "$TEST_CLEAN_LOG" 2>/dev/null | tr '\n' '|')"
    if [[ "$got" == "$want" ]]; then
        pass "$what"
    else
        fail "$what — saw '$got'"
    fi
}

# 4-6b. AXL_SHARED_DRIVER ordering, same contract as assertion 3.
expect_ordered "AXL_SHARED_DRIVER: unload runs before destructors" \
    "CTORSD: unload" "CTORSD: DTOR"

# 7-9. AXL_SERVICE_DRIVER — a different route again, through
#      _axl_service_driver_init, and the ONLY macro with its own separately
#      written copy of the axl_driver_cleanup() call (it does not expand to
#      AXL_DRIVER). Its ordering is therefore not covered by assertion 3:
#      hoisting that call above axl_service_teardown() would pass every other
#      assertion in this file.
expect_count 1 "CTORSVC: setup magic=0x5a5ac0de runs=1 bss=0" \
    "AXL_SERVICE_DRIVER constructs before setup"
expect_count 1 "CTORSVC: DTOR" \
    "AXL_SERVICE_DRIVER runs global destructors on unload"
expect_ordered "AXL_SERVICE_DRIVER: teardown runs before destructors" \
    "CTORSVC: teardown" "CTORSVC: DTOR"

# 10-11. ENTRY FAILURE. The firmware unloads a refused image through
#      CoreUnloadAndCloseImage, which never invokes Unload — so the unload stub
#      does NOT run, and the drain in DriverEntry's failure branch is the only
#      thing that can undo the constructors. RED before that branch existed:
#      the ctor line appeared and the DTOR line did not.
expect_count 1 "CTORFE: entry magic=0x5a5ac0de runs=1 bss=0" \
    "entry-failure image still constructs its globals"
expect_count 1 "CTORFE: DTOR" \
    "entry-failure image DESTRUCTS them (firmware never calls Unload)"
expect_count 0 "CTORFE: UNLOAD-STUB-RAN" \
    "the unload stub is genuinely not called on entry failure"

# 12-13. UNLOAD FAILURE, then a retry. Destructors must NOT run on the refusal
#      — a failing unload leaves the image RESIDENT, and destructed globals
#      under a live image are worse than undestructed ones — and MUST run on
#      the retry, which is what makes success-only a trade rather than a leak.
#      Dropping the `_rc == 0` guard passes every other assertion here.
expect_ordered "unload-failure: no destructors on the refusal, then on the retry" \
    "CTORFU: unload attempt=1" "CTORFU: unload attempt=2" "CTORFU: DTOR"
expect_count 1 "CTORFU: DTOR" \
    "unload-failure image destructs exactly once, on the successful retry"

# 14. The launcher's own leak verdict. This commit adds an allocation to every
#     driver image (the atexit table) and a matching free, driven six times
#     here. Covers the LAUNCHER only — each driver image has its own AxlMem
#     accounting — but that is the half this test can see.
if grep -q "mem: no leaks detected" "$TEST_CLEAN_LOG"; then
    pass "launcher reports no leaks"
else
    fail "launcher leak verdict missing or negative"
    grep -i 'leak' "$TEST_CLEAN_LOG" | head -5 | sed 's/^/      /'
fi

# 15. The launcher itself got through every load/start/unload. Without this a
#    driver that failed to LOAD would print none of the lines above and the
#    counts would simply be wrong, which reads as a constructor bug.
if grep -q "CTOR: FAIL" "$TEST_CLEAN_LOG"; then
    fail "launcher reported a load/start/unload failure"
    grep "CTOR: FAIL" "$TEST_CLEAN_LOG" | sed 's/^/      /'
else
    pass "launcher completed every load/start/unload cycle"
fi

echo ""
printf "cxx-driver-ctors: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -70 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 17 ]] && exit 0 || exit 1
