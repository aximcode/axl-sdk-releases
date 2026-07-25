#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-service-reload-qemu.sh — verify axl_service_reload (the SDK built-in).
#
# `load reload-svc-dxe.efi` starts an AXL_SERVICE_DRIVER service (gen 0) on
# :8080. ~3 s later it calls axl_service_reload(), which abortive-frees :8080
# (its teardown), load+starts a fresh copy (gen 1) that rebinds :8080 and comes
# up resident, then hands off + is reclaimed. Proof: RSVC gen0 + gen1 SETUP, the
# framework's "reload reclaimed old image rc=0x0", and GET :8080/version =
# {"gen":1} — the reloaded service serving on the reused port.
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
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all reload-svc-dxe 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/reload-svc-dxe.efi"

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

echo gen-0 loaded; idling while it self-reloads...
stall 25000000
reset -s
NSHEOF

test_build_image

echo "=== axl_service_reload verification ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "reclaimed old image\|RSVC: gen. reload FAIL\|RSVC: gen. start FAIL" 60; then
    echo "FAIL: self-reload did not complete within 60s"
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

RC=$(grep -oE 'reload reclaimed old image rc=0x[0-9a-fA-F]+' "$TEST_CLEAN_LOG" | head -1 || true)
if [[ "$RC" == "reload reclaimed old image rc=0x0" ]]; then
    pass "framework reclaimed the old image (rc=0x0)"
else
    fail "old image reclaim ('$RC')"
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

[[ $FAIL -eq 0 && $PASS -eq 6 ]] && exit 0 || exit 1
