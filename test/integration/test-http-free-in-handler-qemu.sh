#!/bin/bash
# test-meta: arch=both needs= est=80 local-only=0 guest=standard
# test-http-free-in-handler-qemu.sh -- axl_http_server_free called from inside
# one of the server's own handlers.
#
# A handler runs beneath the server's loop callback, which keeps using the
# connection -- and the server -- after the handler returns. The free used to
# release that memory underneath it (a use-after-free, a write once the
# WebSocket CONNECT deferral landed). It is now completed when the server's
# outermost callback returns, and RESET's promise still holds from there: a
# new server binds the same port at once.
#
# ONE BOOT, seven rounds (AxlTestNet.efi serve-free-in-handler): a fresh server
# on :8080 per round, freed with AXL_TEARDOWN_RESET from inside an ordinary
# route handler, a WebSocket CONNECT, a WebSocket text frame, and an upload
# chunk; and "refree", freed from outside with the WebSocket DISCONNECT that
# teardown fires calling free again (a no-op, not a double free);
# "timer-close", where the app's own timer closes a WebSocket and its
# DISCONNECT frees the server -- outside every server callback; and "run",
# a route handler freeing a server that axl_http_server_run is driving. After
# each, the fixture starts a new server on the same port.
#
# NOT COVERED by a free: the ws_out_on_sent, on_tls_handshake_data and
# on_accept_ready wrappers (no app code runs under the last two; a WS send
# failure whose DISCONNECT frees the server has no deterministic trigger).
#
# CONTROLS: each round's handler really ran and freed (one "handler freed the
# server" line per kind); the rounds are sequenced by the fixture's own
# "ready" lines, so a trigger never reaches the wrong server.
#
# Usage: ./test/integration/test-http-free-in-handler-qemu.sh [--arch X64|AARCH64]

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

# The startup commands live in test-http-free-in-handler-qemu.nsh.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh

test_build_image

echo "=== HTTP: axl_http_server_free from inside a handler ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local desc="$1"; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}
guest_lines() { grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true; }

# ws [text] -- upgrade on /ws; with "text", then send one masked text frame.
ws() {
    python3 - "$HOST_PORT" "${1:-}" << 'PYEOF' > /dev/null 2>&1 || true
import os, socket, sys
port, send_text = int(sys.argv[1]), sys.argv[2] == "text"
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
          b"Connection: Upgrade\r\n"
          b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          b"Sec-WebSocket-Version: 13\r\n\r\n")
buf = b""
try:
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    if send_text:
        payload, mask = b"hi", os.urandom(4)
        s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask +
                  bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
    s.settimeout(3)
    while s.recv(4096):
        pass
except OSError:
    pass
s.close()
PYEOF
}

trigger() {
    case "$1" in
        route)   curl -s -o /dev/null --max-time 5 "http://127.0.0.1:${HOST_PORT}/free" || true ;;
        connect) ws ;;
        frame)   ws text ;;
        upload)  head -c 1000 /dev/zero | curl -s -o /dev/null -X PUT --data-binary @- \
                     --max-time 5 "http://127.0.0.1:${HOST_PORT}/up" || true ;;
        refree|timer-close) ws ;;
        run)     curl -s -o /dev/null --max-time 5 "http://127.0.0.1:${HOST_PORT}/free" || true ;;
    esac
}

KINDS="route connect frame upload refree timer-close run"
# ONE guest runs every round: only the first wait covers its boot (and gets the
# scaled limit); later rounds are bounded waits on a guest that is already up.
_ready_wait=test_wait_boot
for kind in $KINDS; do
    if ! $_ready_wait "FREE: ready $kind" 60; then
        fail "$kind: the fixture did not reach its round within ${TEST_WAITED_S}s"
        break
    fi
    _ready_wait=test_wait_for
    trigger "$kind"
    test_wait_for "FREE: $kind rebind rc=" 30 || true
done
test_wait_for "FREE: DONE" 30 || true
sleep 1
test_clean_log
grep -a '^FREE:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

for kind in $KINDS; do
    if [[ "$kind" == refree ]]; then
        check "refree: CONTROL -- the teardown's DISCONNECT called free again (FREE: refree DISCONNECT freed the server again)" \
            test "$(guest_lines 'FREE: refree DISCONNECT freed the server again')" = 1
    else
        check "$kind: CONTROL -- the handler ran and freed the server (FREE: $kind handler freed the server)" \
            test "$(guest_lines "FREE: $kind handler freed the server")" = 1
    fi
    check "$kind: a new server binds the same port at once (FREE: $kind rebind rc=0)" \
        test "$(guest_lines "FREE: $kind rebind rc=0")" = 1
done
# Whether run returned is the point; its value is axl_loop_run's, which reports
# -1 for ANY quit today (a pre-existing mismatch with its own doc, tracked
# separately), so it is not graded here.
check "run: axl_http_server_run returned once its handler freed the server (FREE: run returned rc=N)" \
    test "$(grep -acE '^FREE: run returned rc=-?[0-9]+$' "$TEST_CLEAN_LOG" || true)" = 1
check "the heap recorded no corruption (FREE: MEM-CORRUPTION 0)" \
    test "$(guest_lines 'FREE: MEM-CORRUPTION 0')" = 1
check "the fixture ran to the end (FREE: DONE)" \
    test "$(guest_lines 'FREE: DONE')" = 1
check "no leaks at teardown" test_check_leaks

echo ""
printf "HTTP free in handler: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
fi
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
