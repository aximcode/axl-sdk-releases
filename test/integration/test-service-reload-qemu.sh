#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0 builds=reload-svc-dxe,reload-svc guest=standard
# test-service-reload-qemu.sh — verify axl_service_reload (the SDK built-in).
#
# `load reload-svc-dxe.efi` starts an AXL_SERVICE_DRIVER service (gen 0) on
# :8080. Then the Shell runs reload-svc.efi, an application: a reload is asked
# for from TPL_APPLICATION, never from the service's own callbacks (UEFI 2.11
# Table 7.3). axl_service_reload loads a fresh copy, stops gen 0 (its teardown
# abortive-frees :8080) and starts gen 1, which rebinds :8080. Proof: RSVC gen0
# + gen1 SETUP, gen 0's TEARDOWN, the launcher's `reload rc=0`, and GET
# :8080/version = {"gen":1} — the reloaded service serving on the reused port.
#
# Usage: ./test/integration/test-service-reload-qemu.sh

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all reload-svc-dxe reload-svc 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/reload-svc-dxe.efi"
test_add_efi "$TEST_BUILD_DIR/reload-svc.efi"

# The startup commands live in test-service-reload-qemu.nsh.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh

test_build_image

echo "=== axl_service_reload verification ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_boot "RSVC: gen. reload rc=\|RSVC: gen. start FAIL" 60; then
    echo "FAIL: the reload did not complete within ${TEST_WAITED_S}s"
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
grep -q "RSVC: gen1 SETUP on :8080" "$TEST_CLEAN_LOG" \
    && pass "service gen-1 (reloaded) rebound :8080 after the abortive teardown" || fail "gen-1 setup"

grep -q "RSVC: gen0 TEARDOWN (abortive)" "$TEST_CLEAN_LOG" \
    && pass "gen-0 was stopped (its teardown ran) before gen-1 bound :8080" \
    || fail "gen-0 teardown"

RC=$(grep -oE 'RSVC: gen0 reload rc=-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$RC" == "RSVC: gen0 reload rc=0" ]]; then
    pass "axl_service_reload from the Shell command returned AXL_OK"
else
    fail "reload rc ('$RC')"
fi

SELF=$(grep -oE 'RSVC: gen0 self-reload rc=-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$SELF" == "RSVC: gen0 self-reload rc=-7" ]]; then
    pass "a reload asked for from inside the service's own image is refused (AXL_UNSUPPORTED)"
else
    fail "self-reload rc ('$SELF')"
fi

MISS=$(grep -oE 'RSVC: gen0 load-miss rc=-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$MISS" == "RSVC: gen0 load-miss rc=-5" ]]; then
    pass "a replacement that cannot be loaded is reported AXL_NOT_FOUND (rc=-5), distinct from the start-failure AXL_ERR"
else
    fail "load-miss rc ('$MISS')"
fi

RESP=$(curl -s -H "Connection: close" --max-time 10 -w "\n%{http_code}" \
            "http://127.0.0.1:${HOST_PORT}/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] \
    && pass "reloaded service serves :8080 (200)" \
    || fail "GET :8080/version after reload (got '$CODE')"
echo "$BODY" | grep -q '"gen":1' \
    && pass "served by the reloaded gen-1 on the reused port" \
    || fail "not gen-1 (body: $BODY)"

echo ""
printf "axl_service_reload: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 8 ]] && exit 0 || exit 1
