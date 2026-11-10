#!/bin/bash
# test-meta: arch=both needs=curl,mtools est=70 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: a staged upgrade across real resets,
#   with the boot disk rewritten (mcopy) between boots.
# test-service-stage-qemu.sh — the staged upgrade, end to end, with real
# resets (sdk/examples/upgrade-svc.c; axl-service.h, "Staged upgrade").
#
# One QEMU run that reboots in place (no -no-reboot), so the disk and NV
# survive each reset as on a real box. startup.nsh runs the launcher on every
# boot; the host drives the service over HTTP:
#   1. v1 serves; GET /version is {"version":"1","last":"none"}
#   2. POST v2 to /upgrade: 202 {"rc":0}; the service resets; the launcher
#      trials v2, waits the grace period and promotes it: the launcher says
#      "staged upgrade applied", and GET /version is {"version":"2","last":"applied"}
#   3. POST a broken image (setup fails): 202; reset; the trial fails, it is
#      quarantined, v2 runs again: {"version":"2","last":"rolled-back"}
#   4. POST the same broken image again: refused, 403 {"rc":-6} (quarantined)
#   5. POST /shutdown; then, on the disk: the image is v2, .bak is v1, .failed
#      is the broken one, and no .new is left.
#
# Usage: ./test/integration/test-service-stage-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" \
    all upgrade-svc-test 2>&1 | tail -2

B="$TEST_BUILD_DIR"
test_add_efi "$B/upgrade-svc-t.efi" upgrade-svc.efi
test_add_efi "$B/upgrade-svc-dxe.efi" upgrade-svc-dxe.efi

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \
connect -r
stall 1000000
ifconfig -s eth0 dhcp
stall 3000000
upgrade-svc.efi start --detach
echo USVC-LAUNCHED
NSHEOF

test_build_image

echo "=== staged upgrade, with resets ($TEST_ARCH) ==="

test_build_qemu_cmd
# Reboot in place: a reset is part of the flow, and the disk must persist.
_cmd=()
for a in "${TEST_QEMU_CMD[@]}"; do [[ "$a" == -no-reboot ]] || _cmd+=("$a"); done
TEST_QEMU_CMD=("${_cmd[@]}")
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# wait_n PATTERN N SECS -- the clean log holds PATTERN (fixed) N times.
wait_n() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        test_clean_log
        (( $(grep -cF -- "$1" "$TEST_CLEAN_LOG" || true) >= $2 )) && return 0
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
get_version() { curl -s --max-time 10 "http://127.0.0.1:$HOST_PORT/version" 2>/dev/null; }
# post_upgrade FILE -- prints "<http code> <body>"
post_upgrade() {
    curl -s --max-time 30 -o "$TEST_TMPDIR/up.body" -w "%{http_code}" \
        -X POST --data-binary @"$1" "http://127.0.0.1:$HOST_PORT/upgrade" 2>/dev/null
    echo " $(cat "$TEST_TMPDIR/up.body" 2>/dev/null)"
}
expect_eq() {   # NAME EXPECTED GOT
    [[ "$3" == "$2" ]] && pass "$1" || fail "$1 (got '$3', expected '$2')"
}

# 1. v1
wait_n "USVC: v1 serving on :8080" 1 120 || { fail "v1 never served"; test_clean_log; tail -30 "$TEST_CLEAN_LOG"; exit 1; }
expect_eq "v1 serves, no upgrade recorded" '{"version":"1","last":"none"}' "$(get_version)"

# 2. stage v2 -> reset -> applied
expect_eq "staging v2 is accepted (202, rc 0)" '202 {"rc":0}' "$(post_upgrade "$B/upgrade-svc-dxe-v2.efi")"
wait_n "USVC: v2 serving on :8080" 1 180 || fail "v2 never served after the reset"
wait_n "upgrade-svc: staged upgrade applied" 1 30 || fail "the launcher did not report the upgrade applied"
expect_eq "after the reset v2 serves, and the upgrade is reported applied" \
    '{"version":"2","last":"applied"}' "$(get_version)"

# 3. stage a broken image -> reset -> rolled back
expect_eq "staging a broken image is accepted (202, rc 0)" '202 {"rc":0}' \
    "$(post_upgrade "$B/upgrade-svc-dxe-broken.efi")"
wait_n "USVC: v3 setup failing on purpose" 1 180 || fail "the broken image was never trialled"
wait_n "USVC: v2 serving on :8080" 2 120 || fail "v2 did not come back after the failed trial"
wait_n "upgrade-svc: staged upgrade rolled back" 1 30 || fail "the launcher did not report the rollback"
expect_eq "after the failed trial v2 serves again, and the rollback is reported" \
    '{"version":"2","last":"rolled-back"}' "$(get_version)"

# 4. the quarantine
expect_eq "staging the same broken image again is refused as quarantined (403, rc -6)" \
    '403 {"rc":-6}' "$(post_upgrade "$B/upgrade-svc-dxe-broken.efi")"
# ...also behind the pack stub: what a stage keys is the image that loads
expect_eq "staging the broken image PACKED is refused as quarantined too (403, rc -6)" \
    '403 {"rc":-6}' "$(post_upgrade "$B/upgrade-svc-dxe-broken-packed.efi")"

# 4b. the image already running: nothing is staged, so nothing resets
expect_eq "staging the running image is a no-op (200, nothing staged)" \
    '200 {"rc":0,"staged":false}' "$(post_upgrade "$B/upgrade-svc-dxe-v2.efi")"
sleep 2
expect_eq "after the no-op v2 still serves -- no reset" \
    '{"version":"2","last":"rolled-back"}' "$(get_version)"
test_clean_log
expect_eq "after the no-op: still the two resets of steps 2 and 3" \
    "2" "$(grep -cF "USVC: resetting to apply the staged upgrade" "$TEST_CLEAN_LOG" || true)"

# 5. shut down, then read the disk
curl -s --max-time 10 -X POST "http://127.0.0.1:$HOST_PORT/shutdown" > /dev/null 2>&1 || true
for ((i = 0; i < 150; i++)); do kill -0 "$TEST_QEMU_PID" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$TEST_QEMU_PID" 2>/dev/null; then
    fail "the guest did not power off"
else
    mcopy -o -i "$TEST_DISK" ::/upgrade-svc-dxe.efi "$TEST_TMPDIR/p.efi" 2>/dev/null
    mcopy -o -i "$TEST_DISK" ::/upgrade-svc-dxe.efi.bak "$TEST_TMPDIR/bak.efi" 2>/dev/null
    mcopy -o -i "$TEST_DISK" ::/upgrade-svc-dxe.efi.failed "$TEST_TMPDIR/failed.efi" 2>/dev/null
    cmp -s "$TEST_TMPDIR/p.efi" "$B/upgrade-svc-dxe-v2.efi" \
        && pass "the image on disk is v2" || fail "the image on disk is not v2"
    cmp -s "$TEST_TMPDIR/bak.efi" "$B/upgrade-svc-dxe.efi" \
        && pass ".bak is v1, byte for byte" || fail ".bak is not v1"
    cmp -s "$TEST_TMPDIR/failed.efi" "$B/upgrade-svc-dxe-broken.efi" \
        && pass ".failed is the broken image, byte for byte" || fail ".failed is not the broken image"
    left=$(mdir -i "$TEST_DISK" -b ::/ 2>/dev/null | grep -ciE 'upgrade-svc-dxe\.efi\.new|~AXL.*\.TMP' || true)
    expect_eq "no staged image or aside is left" "0" "$left"
fi

echo ""
printf "staged upgrade: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    test_clean_log; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
fi
[[ $FAIL -eq 0 && $PASS -eq 14 ]] && exit 0 || exit 1
