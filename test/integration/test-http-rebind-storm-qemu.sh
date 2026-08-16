#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0
# test-http-rebind-storm-qemu.sh — AXL_TEARDOWN_RESET must return BOUNDED under
# a connect-storm.
#
# Regression for the f558bc5d accept-backlog drain racing the inbound SYN rate:
# when a peer hammers NEW connections continuously THROUGH the teardown, the
# firmware keeps refilling the accept backlog while the drain runs, so a drain
# that re-arms Accept and chases fresh arrivals never converges and the RESET
# free wedges (>10 s / never returns).
#
# AxlTestNet.efi serve-rebind-storm: serves :8080, ~4 s in brackets the RESET
# free with FREE-START / FREE-DONE, then rebinds. The host storms connect() the
# whole time. The free MUST reach FREE-DONE within a tight bound.
#   fixed (GREEN): FREE-DONE within the bound + REBIND-RC:0 (rebindable).
#   buggy (RED): FREE-DONE never arrives in the bound — the free wedged.
#
# Usage: ./test/integration/test-http-rebind-storm-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
FREE_BOUND_S="${FREE_BOUND_S:-8}"     # max seconds for the RESET free to return
STORMERS="${STORMERS:-4}"

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting serve-rebind-storm...
AxlTestNet.efi serve-rebind-storm
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== AXL_TEARDOWN_RESET bounded under connect-storm ($TEST_ARCH, stormers=$STORMERS, bound=${FREE_BOUND_S}s) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# Continuous connect-storm: open connections as fast as possible and abandon
# them, spanning the ~4 s until the guest frees AND the whole free window, so
# the firmware backlog is being refilled while the drain runs.
STORM_STOP="$TEST_TMPDIR/storm.stop"
for _s in $(seq 1 "$STORMERS"); do
python3 - "$HOST_PORT" "$STORM_STOP" << 'PYEOF' &
import socket, sys, os, time
port = int(sys.argv[1]); stop = sys.argv[2]
deadline = time.time() + 16
while time.time() < deadline and not os.path.exists(stop):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setblocking(False)
        try: s.connect(("127.0.0.1", port))
        except BlockingIOError: pass
        # abandon almost immediately — maximize fresh SYNs, never deliver a request
        s.close()
    except Exception:
        pass
PYEOF
done
STORM_PIDS=$(jobs -p)

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# The free is bracketed by FREE-START / FREE-DONE. Wait for FREE-START (generous),
# then require FREE-DONE within a TIGHT bound — that bound IS the "returns
# bounded" assertion (a wedged free never reaches FREE-DONE in time).
if test_wait_for "FREE-START" 60; then
    if test_wait_for "FREE-DONE" "$FREE_BOUND_S"; then
        pass "AXL_TEARDOWN_RESET free returned within ${FREE_BOUND_S}s under the storm"
    else
        fail "RESET free did NOT return within ${FREE_BOUND_S}s — wedged by the connect-storm"
    fi
else
    fail "never reached FREE-START"
fi

# Port should also be rebindable (best-effort under a live storm).
if test_wait_for "REBIND-RC:" 20; then
    test_clean_log
    RC=$(grep -oE 'REBIND-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1)
    [[ "$RC" == "REBIND-RC:0" ]] \
        && pass "port rebindable with the storm running ($RC)" \
        || fail "port not rebindable under storm ($RC)"
else
    fail "no REBIND-RC (free likely still wedged)"
fi

touch "$STORM_STOP" 2>/dev/null
kill $STORM_PIDS 2>/dev/null

echo ""
printf "RESET bounded under connect-storm: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then echo ""; echo "--- Serial ---"; test_clean_log; tail -40 "$TEST_CLEAN_LOG"; fi
[[ $FAIL -eq 0 && $PASS -eq 2 ]] && exit 0 || exit 1
