#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-http-rebind-load-qemu.sh — port-releasing teardown UNDER LOAD.
#
# Follow-up to test-http-rebind-qemu.sh. That test held ONE accepted, delivered
# connection; abortive teardown released the port. This test holds SEVERAL
# connections that are in the firmware ACCEPT BACKLOG — established but never
# delivered to the app (the guest never pumps its accept loop before teardown),
# so the server tracks none of them. Those backlog children hold PCBs on the
# port, and a correct abortive teardown must RST + DestroyChild them
# synchronously so an immediate same-process rebind (no pump) succeeds.
#
# AxlTestNet.efi serve-rebind-load <graceful|abortive>:
#   READY -> host opens N backlog connections -> guest sleeps (firmware
#   completes the handshakes) -> free + rebind :8080 synchronously.
#     abortive (GREEN): REBIND-RC == 0 + the rebound server answers a GET.
#     abortive, unfixed (RED): REBIND-RC != 0 — the backlog still holds the port.
#
# REBIND_VARIANT=graceful runs the graceful comparison. Default: abortive.
# BACKLOG_N (default 6) controls how many backlog connections to open.
#
# Usage: ./test/integration/test-http-rebind-load-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
VARIANT="${REBIND_VARIANT:-abortive}"
BACKLOG_N="${BACKLOG_N:-6}"

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

echo Starting serve-rebind-load (${VARIANT})...
AxlTestNet.efi serve-rebind-load ${VARIANT}
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== HTTP server port-release under load ($TEST_ARCH, variant=$VARIANT, N=$BACKLOG_N) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# On READY: open N connections and hold them open with a partial (never
# terminated) request. The guest is sleeping (not pumping accept), so the
# firmware completes each handshake and queues it in the accept BACKLOG,
# undelivered. Hold them ~9 s so they are all in the backlog at teardown.
python3 - "$HOST_PORT" "$BACKLOG_N" << 'PYEOF' &
import socket, sys, time
port = int(sys.argv[1]); n = int(sys.argv[2])
socks = []
for _ in range(n):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /plain HTTP/1.1\r\nHost: x\r\n")   # no terminator: in-flight
        socks.append(s)
    except Exception:
        pass
time.sleep(9)
for s in socks:
    try:
        s.close()
    except Exception:
        pass
PYEOF
LOAD_PID=$!

# The guest sleeps ~4 s after READY, then frees + rebinds.
if ! test_wait_for "REBIND-RC:" 60; then
    echo "FAIL: no REBIND-RC within 60s"
    kill "$LOAD_PID" 2>/dev/null
    test_clean_log; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
RC=$(grep -oE 'REBIND-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1)

if [[ "$VARIANT" == "abortive" ]]; then
    [[ "$RC" == "REBIND-RC:0" ]] \
        && pass "abortive teardown released :8080 under a $BACKLOG_N-deep backlog ($RC)" \
        || fail "abortive port-release under load ($RC — backlog still holds the port)"

    # Prove the rebound server actually serves on the reused port.
    if test_wait_for "READY2" 20; then
        RESP=$(curl -s -H "Connection: close" --max-time 10 -w "\n%{http_code}" \
                    "http://127.0.0.1:${HOST_PORT}/plain" 2>/dev/null || true)
        CODE=$(echo "$RESP" | tail -1)
        [[ "$CODE" == "200" ]] \
            && pass "rebound server answers a fresh GET on the reused port (200)" \
            || fail "rebound server GET (got '$CODE')"
    else
        fail "rebound server never reached READY2"
    fi
else
    # Graceful comparison: with no pump, the deferred close leaves the port held.
    [[ "$RC" != "REBIND-RC:0" ]] \
        && pass "graceful teardown leaves :8080 held under load ($RC) — expected" \
        || fail "graceful unexpectedly released the port ($RC)"
fi

kill "$LOAD_PID" 2>/dev/null

echo ""
printf "port-release under load: %d passed, %d failed (%s, %s)\n" "$PASS" "$FAIL" "$TEST_ARCH" "$VARIANT"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
