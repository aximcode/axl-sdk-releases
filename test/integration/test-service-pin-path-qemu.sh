#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-service-pin-path-qemu.sh — AxlServiceDeploy.driver_path pins the
# driver image: exactly one file, no 4-path search, no embedded fallback.
#
# Two builds of the SAME service (same name, same derived GUID) are staged in
# the two slots the default search walks, plus a third copy embedded in the
# launcher:
#
#   \pin-svc-dxe.efi                    GOOD build   (search candidate #4 —
#                                       a launcher at the volume root has no
#                                       usable image directory, so its own
#                                       sibling sorts AFTER drivers/<arch>/)
#   \drivers\<arch>\pin-svc-dxe.efi     SHADOW build (search candidate #2)
#   embedded in the launcher            SHADOW build (last-resort fallback)
#
# Which one the default search picks is **arch-dependent** — under OVMF/X64 it
# is the stale drivers/<arch>/ copy (exactly the reported hazard: a stale
# drivers/<arch>/<name> beating the copy the launcher staged), under
# AARCH64 it is the root sibling. So the test does not assert the default
# order; it pins BOTH copies in turn and asserts each pin wins. On either
# arch one of those two pins contradicts what the default search would have
# chosen, which is the property that matters.
#
# Runs, in one boot:
#   1. default resolution                  -> whichever the search picks
#   2. pinned to drivers/<arch>/ copy      -> variant=shadow
#   3. pinned to the root copy             -> variant=good
#   4. pinned to a file that does not exist -> rc=-5 and NO fourth SETUP line
#                                             (the embedded shadow was NOT used)
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-service-pin-path-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
declare -A _ARCH_DIR_MAP=([X64]=x64 [AARCH64]=aarch64)
_arch_dir="${_ARCH_DIR_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    pin-svc 2>&1 | tail -2

test_add_efi "$NATIVE_DIR/pin-svc-launcher.efi"      "pin-svc-launcher.efi"
test_add_efi "$NATIVE_DIR/pin-svc-driver-good.efi"   "pin-svc-dxe.efi"
test_add_efi "$NATIVE_DIR/pin-svc-driver-shadow.efi" "drivers/$_arch_dir/pin-svc-dxe.efi"

PIN_SHADOW="fs0:\\drivers\\$_arch_dir\\pin-svc-dxe.efi"
PIN_GOOD="fs0:\\pin-svc-dxe.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo RUN1_DEFAULT"
    echo "pin-svc-launcher.efi start"
    echo "pin-svc-launcher.efi stop"
    echo "echo RUN2_PIN_SHADOW"
    echo "pin-svc-launcher.efi start $PIN_SHADOW"
    echo "pin-svc-launcher.efi stop"
    echo "echo RUN3_PIN_GOOD"
    echo "pin-svc-launcher.efi start $PIN_GOOD"
    echo "pin-svc-launcher.efi stop"
    echo "echo RUN4_PIN_MISSING"
    echo "pin-svc-launcher.efi start fs0:\\pin-svc-missing.efi"
    echo "echo PINSVC_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlServiceDeploy.driver_path pin ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

if ! test_wait_for "PINSVC_DONE" 90; then
    echo "FAIL: fixture did not finish within 90s"
    test_clean_log; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

# The SETUP lines, in order, tell the whole story.
mapfile -t SETUPS < <(grep -oE 'PINSVC: variant=[a-z]+ SETUP' "$TEST_CLEAN_LOG" || true)

case "${SETUPS[0]:-}" in
    "PINSVC: variant=shadow SETUP")
        pass "default resolution picked the stale drivers/<arch>/ copy (the reported hazard)" ;;
    "PINSVC: variant=good SETUP")
        pass "default resolution picked the root sibling" ;;
    *)
        fail "run 1 produced no service ('${SETUPS[0]:-<none>}')" ;;
esac

[[ "${SETUPS[1]:-}" == "PINSVC: variant=shadow SETUP" ]] \
    && pass "driver_path=drivers/<arch>/ loads that exact copy" \
    || fail "run 2 setup ('${SETUPS[1]:-<none>}')"

[[ "${SETUPS[2]:-}" == "PINSVC: variant=good SETUP" ]] \
    && pass "driver_path=root loads that exact copy" \
    || fail "run 3 setup ('${SETUPS[2]:-<none>}')"

# NOTE: no assertion here that "a pin beat the search". The two assertions
# above pin SETUPS[1] and SETUPS[2] to two DIFFERENT exact strings, so
# SETUPS[0] cannot equal both -- one of the pins necessarily contradicted the
# search result already. An extra check would read as coverage and add none.

RC=$(grep -oE 'PINSVC: launcher start rc=-?[0-9]+' "$TEST_CLEAN_LOG" | tail -1 || true)
[[ "$RC" == "PINSVC: launcher start rc=-5" ]] \
    && pass "a pinned path that does not exist reports AXL_NOT_FOUND" \
    || fail "run 4 rc ('$RC')"

[[ "${#SETUPS[@]}" -eq 3 ]] \
    && pass "no fourth service came up — driver_path suppressed the embedded fallback" \
    || fail "expected exactly 3 SETUP lines, saw ${#SETUPS[@]}"

echo ""
printf "AxlServiceDeploy.driver_path: %d passed, %d failed (%s)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -70 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 5 ]] && exit 0 || exit 1
