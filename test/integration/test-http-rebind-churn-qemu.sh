#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-http-rebind-churn-qemu.sh — port-release with PENDING DEFERRED CLOSES.
#
# Third port-holder category after test-http-rebind-qemu.sh (single live conn)
# and test-http-rebind-load-qemu.sh (accept backlog): connections that were
# accepted and SERVED, then gracefully closed (Connection: close), whose close
# is still in flight at teardown. Their s->conns slot is already freed and their
# AxlTcpCloseCtx / on_close_event is owned by the LOOP, not the server, so the
# abortive teardown never saw them — they hold a PCB on the port (until ~2 s
# TIME_WAIT) and a caller-owned loop source.
#
# AxlTestNet.efi serve-rebind-churn <graceful|abortive>: pumps ~1.2 s to serve +
# graceful-close the host's churn connections (deferred closes in flight), then
# frees + rebinds :8080 synchronously with NO further pump, then frees the loop.
#   abortive (GREEN): REBIND-RC == 0 AND no "caller-owned event source still
#                     active" at axl_loop_free.
#   abortive, unfixed (RED): REBIND-RC != 0 and the deferred-close sources leak.
#
# REBIND_VARIANT=graceful runs the graceful comparison. CHURN_N (default 6).
#
# Usage: ./test/integration/test-http-rebind-churn-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
VARIANT="${REBIND_VARIANT:-abortive}"
CHURN_N="${CHURN_N:-6}"

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

echo Starting serve-rebind-churn (${VARIANT})...
AxlTestNet.efi serve-rebind-churn ${VARIANT}
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== HTTP server port-release with pending deferred closes ($TEST_ARCH, variant=$VARIANT, N=$CHURN_N) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# On READY: open N connections, send a COMPLETE Connection: close request on each
# and read the response. The server serves each and then GRACEFULLY closes it
# (Connection: close), leaving a deferred close in flight. Keep the sockets open
# briefly so the closes are still in TIME_WAIT (undelivered) at teardown.
python3 - "$HOST_PORT" "$CHURN_N" << 'PYEOF' &
import socket, sys, time
port = int(sys.argv[1]); n = int(sys.argv[2])
socks = []
for _ in range(n):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        try:
            s.recv(4096)     # let the server send its response + start closing
        except Exception:
            pass
        socks.append(s)
    except Exception:
        pass
time.sleep(6)
for s in socks:
    try:
        s.close()
    except Exception:
        pass
PYEOF
CHURN_PID=$!

if ! test_wait_for "REBIND-RC:" 60; then
    echo "FAIL: no REBIND-RC within 60s"
    kill "$CHURN_PID" 2>/dev/null
    test_clean_log; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
test_wait_for "REBIND-DONE" 20 || true
kill "$CHURN_PID" 2>/dev/null

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
RC=$(grep -oE 'REBIND-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1)

if [[ "$VARIANT" == "abortive" ]]; then
    [[ "$RC" == "REBIND-RC:0" ]] \
        && pass "abortive teardown released :8080 with $CHURN_N deferred closes in flight ($RC)" \
        || fail "abortive port-release with pending closes ($RC — deferred closes still hold the port)"

    grep -q "caller-owned event source .* still active" "$TEST_CLEAN_LOG" \
        && fail "abortive teardown left deferred-close sources active at loop_free" \
        || pass "no still-active caller-owned source at loop_free (deferred closes finalized)"
else
    [[ "$RC" != "REBIND-RC:0" ]] \
        && pass "graceful teardown leaves :8080 held with pending closes ($RC) — expected" \
        || fail "graceful unexpectedly released the port ($RC)"
fi

echo ""
printf "port-release with pending closes: %d passed, %d failed (%s, %s)\n" "$PASS" "$FAIL" "$TEST_ARCH" "$VARIANT"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
