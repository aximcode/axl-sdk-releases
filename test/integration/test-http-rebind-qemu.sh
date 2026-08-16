#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0
# test-http-rebind-qemu.sh — port-releasing HTTP-server teardown.
#
# Proves axl_http_server_free(RESET) releases the listen port SYNCHRONOUSLY:
# after it returns, an immediate rebind on the same port succeeds even with an
# in-flight connection and WITHOUT pumping the loop. This is the SoftBMC
# self-re-exec case (parent frees its :443 server, then blocks in StartImage —
# the graceful+deferred close never finalizes and the child can't rebind).
#
# AxlTestNet.efi serve-rebind <graceful|abortive> starts an HTTP server on 8080,
# and ~2 s later — from inside a loop callback (so the graceful free defers) —
# frees it and rebinds 8080 synchronously, printing REBIND-RC. The harness holds
# an in-flight connection open across that teardown.
#   graceful (RED): REBIND-RC != 0 — port still held (reproduces the bug)
#   abortive (GREEN): REBIND-RC == 0 + the rebound server answers a fresh GET
#
# REBIND_VARIANT=graceful runs the RED reproduction. Default: abortive.
#
# Usage: ./test/integration/test-http-rebind-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
VARIANT="${REBIND_VARIANT:-abortive}"

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

echo Starting serve-rebind (${VARIANT})...
AxlTestNet.efi serve-rebind ${VARIANT}
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== HTTP server port-releasing teardown ($TEST_ARCH, variant=$VARIANT) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# Hold an in-flight connection open ACROSS the ~2 s teardown: connect to the
# server, send a partial (never-terminated) request, and keep the socket open
# for ~8 s so a live connection exists at free time.
python3 - "$HOST_PORT" << 'PYEOF' &
import socket, sys, time
port = int(sys.argv[1])
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(b"GET /plain HTTP/1.1\r\nHost: x\r\n")   # no terminator: in-flight
    time.sleep(8)
    s.close()
except Exception:
    pass
PYEOF
INFLIGHT_PID=$!

# Wait for the rebind to happen (REBIND-RC line), then let the server settle.
test_wait_for "REBIND-RC:" 30 || true
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
RC=$(grep -oE 'REBIND-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 | sed 's/.*://')

if [[ "$VARIANT" == "abortive" ]]; then
    if [[ "$RC" == "0" ]]; then
        pass "abortive teardown: port released, rebind on :8080 succeeded (rc=0)"
    else
        fail "abortive teardown: rebind failed (REBIND-RC='${RC:-<absent>}', want 0)"
    fi
    # The rebound server must actually answer on the reused port.
    RESP=$(curl -s -H "Connection: close" --max-time 10 -w "\n%{http_code}" \
                "http://127.0.0.1:${HOST_PORT}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "rebound server answers GET /plain on the reused port (200)" \
        || fail "rebound server GET /plain (got '$CODE')"
    grep -q "caller-owned event source .* still active" "$TEST_CLEAN_LOG" \
        && fail "abortive teardown left a deferred close source (still active at loop_free)" \
        || pass "no still-active caller-owned source at loop_free"
else
    # RED reproduction: graceful free while the loop is running defers the
    # listener finalize, so the immediate rebind must FAIL.
    if [[ -n "$RC" && "$RC" != "0" ]]; then
        pass "graceful teardown reproduces the bug: rebind failed (rc=$RC) — port held"
    else
        fail "graceful teardown UNEXPECTEDLY rebound (REBIND-RC='${RC:-<absent>}') — test not reproducing the race"
    fi
fi

wait "$INFLIGHT_PID" 2>/dev/null || true

echo ""
printf "HTTP rebind teardown (%s): %d passed, %d failed (%s)\n" "$VARIANT" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
