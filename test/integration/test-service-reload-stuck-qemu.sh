#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0 builds=reload-svc-dxe,reload-svc,reload-svc-stuck-dxe guest=standard
# test-service-reload-stuck-qemu.sh — axl_service_reload against a service
# that will not stop.
#
# reload-svc-stuck-dxe.efi publishes the "reload-svc" service GUID and refuses
# its unload, as an image with no Unload handler does. The Shell then runs
# reload-svc.efi: the replacement (reload-svc-dxe.efi) loads, the stop fails,
# and the service is STILL published -- still serving. That is not "down":
# the reload must say AXL_DENIED (-6), unload the replacement it loaded, and
# never start it. RED (before): AXL_ERR (-1), the code that tells a caller to
# roll back and reset a box that never stopped serving.
#
# Usage: ./test/integration/test-service-reload-stuck-qemu.sh

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" \
    all reload-svc-dxe reload-svc reload-svc-stuck-dxe 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/reload-svc-dxe.efi"
test_add_efi "$TEST_BUILD_DIR/reload-svc.efi"
test_add_efi "$TEST_BUILD_DIR/reload-svc-stuck-dxe.efi"

# The startup commands live in test-service-reload-stuck-qemu.nsh.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh

test_build_image

echo "=== axl_service_reload: a service that will not stop ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID"

if ! test_wait_boot "RELOAD-DONE" 60; then
    echo "FAIL: the reload did not finish within ${TEST_WAITED_S}s"
    test_clean_log; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
grep -q "RSVCS: published, will not unload" "$TEST_CLEAN_LOG" \
    && pass "the stuck service published the reload-svc GUID" || fail "stuck service setup"

grep -q "RSVCS: refusing unload" "$TEST_CLEAN_LOG" \
    && pass "the reload tried to stop it, and its unload refused" \
    || fail "the stop was never attempted"

RC=$(grep -oE 'RSVC: gen0 reload rc=-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$RC" == "RSVC: gen0 reload rc=-6" ]]; then
    pass "a service still published after a failed stop is AXL_DENIED (rc=-6), not the fatal AXL_ERR"
else
    fail "reload rc ('$RC'; rc=-1 is the false 'down')"
fi

grep -q "RSVC: gen1 SETUP" "$TEST_CLEAN_LOG" \
    && fail "the replacement was started over a service that is still serving" \
    || pass "the replacement was never started"

echo ""
printf "axl_service_reload stuck service: %d passed, %d failed (%s)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 4 ]] && exit 0 || exit 1
