#!/bin/bash
# test-meta: arch=both needs= est=20 local-only=0 guest=standard
# test-ws-held-101-qemu.sh -- a WebSocket connection torn down while its 101's
# completion callback is still owed must not fire AXL_WS_CONNECT.
#
# axl_tcp_close delivers every send callback it still holds before it
# returns, and one whose send had already FINISHED arrives with its real
# status, AXL_OK -- not AXL_CANCELLED. The HTTP server fires the app's
# AXL_WS_CONNECT from the 101's completion. Its teardown checked
# ws_connected (to decide on DISCONNECT) and only THEN closed the socket, so
# an owed 101 delivered by that close completed the upgrade from inside the
# teardown: CONNECT fired, DISCONNECT never did, and the slot the app had
# just been handed was zeroed for reuse by the next client.
#
# THE WINDOW IS ONE LOOP ITERATION WIDE in the ordinary case, so it is held
# open with a seam: AxlTestNet.efi serve-ws-held-101 arms
# axl_tcp_fail_next_send_schedule(1) before its server starts, and the
# host's FIRST request is the upgrade, so the 101's completion is the one
# held. The client reads the 101 and resets; GET /stop then ends the loop,
# and the server is freed with the upgraded connection still in its table.
#
# THE CONTRACT: 0 CONNECT / 0 DISCONNECT. A connection torn down before its
# upgrade completed was never the app's -- the same rule that already gives a
# 101 whose send FAILED no CONNECT and no DISCONNECT. (sshd parity: a session
# that is gone before it was established never reaches the session hooks.)
#
# CONTROLS: the client received the 101 (the send really finished), the seam
# held exactly one completion and it was not a real both-pools-full hold, and
# no CONNECT had fired by GET /stop -- unheld, it runs on the very next
# iteration, so connect=0 there is what says the held completion was the 101.
#
# NOT COVERED: axl_http_server_free(GRACEFUL). Its finalize is parked on the
# loop, which is not pumped after axl_loop_run returns, so the leak gate would
# grade that instead of the defect; the guard runs on the same path either way.
#
# Usage: ./test/integration/test-ws-held-101-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# The startup commands live in test-ws-held-101-qemu.nsh.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh

test_build_image

echo "=== HTTP: WS connection torn down with its 101 completion owed ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_boot "READY" 60; then
    echo "FAIL: server did not reach READY within ${TEST_WAITED_S} seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# The upgrade, then an abortive close once the 101 has been read. Prints the
# 101's status line exactly as received.
CLIENT_OUT="$TEST_TMPDIR/ws-held-client.out"
python3 - "$HOST_PORT" > "$CLIENT_OUT" 2>&1 << 'PYEOF' || true
import socket, struct, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.sendall(b"GET /ws-held HTTP/1.1\r\nHost: x\r\n"
          b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
          b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          b"Sec-WebSocket-Version: 13\r\n\r\n")
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
print("CLIENT: " + buf.split(b"\r\n", 1)[0].decode("latin-1"))
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
PYEOF
sleep 1

STOP_BODY=$(curl -s -H "Connection: close" --max-time 10 \
                 "http://127.0.0.1:${HOST_PORT}/stop" 2>/dev/null || true)

test_wait_for "WS-HELD: DONE" 30 || true
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# check <description> <command...>
check() {
    local desc="$1"; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}
# Exact whole-line occurrences in the guest log.
guest_lines() {
    grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true
}
# Occurrences of a substring (a log line carries a prefix of its own).
guest_count() {
    grep -acF -- "$1" "$TEST_CLEAN_LOG" || true
}

HELD_INJECTED='async send: completion held (injected by axl_tcp_fail_next_send_schedule)'
HELD_INJECTED_OK="$HELD_INJECTED, status 0"
HELD_REAL='async send: cannot schedule the completion callback'

test_clean_log
echo "  client: '$(tr '\n' '|' < "$CLIENT_OUT")'; /stop body '$STOP_BODY';" \
     "injected holds $(guest_count "$HELD_INJECTED");" \
     "real both-full $(guest_count "$HELD_REAL")"
grep -a '^WS-HELD:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

check "CONTROL: the client received the 101 (the send finished)" \
    test "$(grep -cxF 'CLIENT: HTTP/1.1 101 Switching Protocols' "$CLIENT_OUT" || true)" = 1
check "CONTROL: the seam held exactly one completion, a finished one (status 0)" \
    test "$(guest_count "$HELD_INJECTED_OK")/$(guest_count "$HELD_INJECTED")" = 1/1
check "CONTROL: no REAL both-pools-full hold happened" \
    test "$(guest_count "$HELD_REAL")" = 0
check "CONTROL: no CONNECT before teardown -- the held completion was the 101's (WS-HELD: before-free connect=0 disconnect=0)" \
    test "$(guest_lines 'WS-HELD: before-free connect=0 disconnect=0')" = 1
check "GET /stop answered 'stopping'" \
    test "$STOP_BODY" = "stopping"
check "teardown with the 101 owed fires neither CONNECT nor DISCONNECT (WS-HELD: after-free connect=0 disconnect=0)" \
    test "$(guest_lines 'WS-HELD: after-free connect=0 disconnect=0')" = 1
check "no CONNECT was delivered at all (WS-HELD: CONNECT x0)" \
    test "$(guest_lines 'WS-HELD: CONNECT')" = 0
check "the heap recorded no corruption (WS-HELD: MEM-CORRUPTION 0)" \
    test "$(guest_lines 'WS-HELD: MEM-CORRUPTION 0')" = 1
check "the fixture ran to the end (WS-HELD: DONE)" \
    test "$(guest_lines 'WS-HELD: DONE')" = 1
check "no leaks at teardown" test_check_leaks

echo ""
printf "HTTP WS held-101 teardown: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
