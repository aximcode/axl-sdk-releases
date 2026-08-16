#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-service-reload-fail-qemu.sh — axl_service_reload must report a
# replacement that LOADS but never STARTS.
#
# Same gen-0 service as test-service-reload-qemu.sh, but this image also
# stages reload-svc-fail-dxe.efi — a poisoned replacement publishing the same
# service name whose setup returns AXL_ERR on purpose. Staging that file is
# what selects this scenario inside the fixture.
#
# The framework declines to attach the replacement and its DriverEntry returns
# an EFI error. RED (before the S0 fix): _axl_service_driver_init narrowed the
# EFI_STATUS to `int`, dropping EFI_ERROR_BIT, so StartImage reported success
# and the fixture printed `start-fail rc=0` — a successful hot-swap of a
# service that never came up. GREEN: `start-fail rc=-1`, and no gen-1 setup.
#
# Usage: ./test/integration/test-service-reload-fail-qemu.sh

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all reload-svc-dxe reload-svc-fail-dxe 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/reload-svc-dxe.efi"
test_add_efi "$TEST_BUILD_DIR/reload-svc-fail-dxe.efi"

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Loading gen-0 service...
load reload-svc-dxe.efi

echo gen-0 loaded; idling while it attempts the poisoned reload...
stall 20000000
reset -s
NSHEOF

test_build_image

echo "=== axl_service_reload start-failure reporting ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID"

if ! test_wait_for "RSVC: gen. start-fail rc=" 60; then
    echo "FAIL: fixture never reported a start-failure rc within 60s"
    test_clean_log; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 2

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
grep -q "RSVC: gen0 SETUP on :8080" "$TEST_CLEAN_LOG" \
    && pass "service gen-0 came up on :8080" || fail "gen-0 setup"

grep -q "RSVCF: poisoned setup failing on purpose" "$TEST_CLEAN_LOG" \
    && pass "poisoned replacement was loaded and its setup ran" \
    || fail "poisoned replacement setup never ran"

grep -q "service 'reload-svc': setup returned -1 - not attaching" "$TEST_CLEAN_LOG" \
    && pass "framework declined to attach the poisoned replacement" \
    || fail "framework attach-decline warning missing"

RC=$(grep -oE 'RSVC: gen0 start-fail rc=-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$RC" == "RSVC: gen0 start-fail rc=-1" ]]; then
    pass "axl_service_reload reported the start failure (rc=-1)"
else
    fail "axl_service_reload start-failure rc ('$RC'; rc=0 is the S0 bug)"
fi

grep -q "RSVC: gen1 SETUP" "$TEST_CLEAN_LOG" \
    && fail "a gen-1 service came up (it must not — setup was poisoned)" \
    || pass "no replacement service came up"

echo ""
printf "axl_service_reload start-failure: %d passed, %d failed (%s)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 5 ]] && exit 0 || exit 1
