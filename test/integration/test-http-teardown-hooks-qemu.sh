#!/bin/bash
# test-meta: arch=both needs= est=120 local-only=0 guest=standard
# test-http-teardown-hooks-qemu.sh -- the HTTP server's application hooks fire
# exactly once at teardown: a WebSocket's DISCONNECT, and an upload's
# terminating call.
#
# BOOT 1, serve-ws-close-in-connect. A CONNECT handler that closes its own
# connection (axl_ws_conn_close) and returns AXL_OK has ACCEPTED it, so it
# gets DISCONNECT -- after its CONNECT returns, never nested inside it. One
# that closes and returns AXL_ERR has REJECTED it: no DISCONNECT. The server
# used to give the accepting handler no DISCONNECT at all (ws_connected was
# set only after CONNECT returned), leaking whatever it registered.
#
# BOOT 2, serve-upload-held. AxlUploadHandler promises its clean-EOF and
# abort calls are mutually exclusive. The connection stayed marked as an
# upload until the response's completion ran, so a teardown in between called
# the handler again with aborted=true after it had answered 201 -- a handler
# whose abort path deletes a partial file would delete the finished one. The
# window is held open with axl_tcp_fail_next_send_schedule(1): the upload is
# the boot's first request, so its response's completion is the one held.
#
# BOOT 3, serve-upload-held fail. A chunk call that returns AXL_ERR ends the
# upload as clean EOF does; the same teardown window must not add an abort
# call to it either.
#
# BOOT 4, the same with a body of several chunks: the rest of the body must be
# discarded -- no further chunk call, no clean-EOF call, no second response.
#
# CONTROLS: boot 1 -- both upgrades received their 101 and both CONNECTs
# fired (the handlers really ran); boot 2 -- the client got the 201, the
# handler saw every byte and exactly one clean EOF before teardown, and the
# seam held exactly one completion, with status 0 (a finished send, not a
# failed one), and no real both-pools-full hold happened.
#
# NOT COVERED: axl_http_server_free(GRACEFUL). Its finalize is parked on the
# loop, which is not pumped after axl_loop_run returns, so the leak gate would
# grade that instead; the hooks run on the same path either way.
#
# Usage: ./test/integration/test-http-teardown-hooks-qemu.sh [--arch X64|AARCH64]

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

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local desc="$1"; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}
guest_lines() { grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true; }
guest_count() { grep -acF -- "$1" "$TEST_CLEAN_LOG" || true; }

# boot MODE -- start AxlTestNet.efi MODE (test-http-teardown-hooks-qemu.nsh)
# and wait for READY. Each scenario a fresh machine: a new boot, or -- pooled
# -- the leased guest restored (common-test.sh, a background run after
# test_guest_stop).
boot() {
    local -a words
    read -ra words <<< "$1"     # a mode and its argument: %1 %2 in the .nsh
    test_use_nsh "${words[@]}"
    test_build_image
    test_build_qemu_cmd
    test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
    test_run_background
    if ! test_wait_boot "READY" 60; then
        fail "$1: server did not reach READY within ${TEST_WAITED_S} s"
        test_clean_log; tail -30 "$TEST_CLEAN_LOG"
        return 1
    fi
}

# ws_upgrade PATH -- upgrade, print the 101's status line, then wait for the
# server's close (it closes from its CONNECT handler).
ws_upgrade() {
    python3 - "$HOST_PORT" "$1" << 'PYEOF' 2>&1 || true
import socket, sys
port, path = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n" % path).encode())
buf = b""
try:
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    print("CLIENT: " + buf.split(b"\r\n", 1)[0].decode("latin-1"))
    while s.recv(4096):
        pass
except OSError:
    pass
s.close()
PYEOF
}

# end_boot -- stop this boot's guest and start the next one on an empty
# serial log (test_cleanup would also delete the tmpdir the next boot needs;
# a stale log would satisfy the next "READY" wait and pollute its counts).
end_boot() {
    test_cpu_check || true
    test_guest_stop
    : > "$TEST_LOG"
}

stop() {
    curl -s -H "Connection: close" --max-time 10 \
        "http://127.0.0.1:${HOST_PORT}/stop" 2>/dev/null || true
}

echo "=== HTTP teardown hooks ($TEST_ARCH) ==="

# ---- boot 1: a CONNECT handler that closes its own connection -------------
if boot serve-ws-close-in-connect; then
    OK_OUT=$(ws_upgrade /ws-close-ok)
    REJ_OUT=$(ws_upgrade /ws-close-reject)
    STOP_BODY=$(stop)
    test_wait_for "WS-CIC: DONE" 30 || true
    test_clean_log
    echo "  ok client '$OK_OUT'; reject client '$REJ_OUT'; /stop '$STOP_BODY'"
    grep -a '^WS-CIC:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

    check "CONTROL: /ws-close-ok answered 101" \
        test "$OK_OUT" = "CLIENT: HTTP/1.1 101 Switching Protocols"
    check "CONTROL: /ws-close-reject answered 101" \
        test "$REJ_OUT" = "CLIENT: HTTP/1.1 101 Switching Protocols"
    check "CONTROL: both CONNECT handlers ran once (WS-CIC: ok CONNECT / reject CONNECT)" \
        test "$(guest_lines 'WS-CIC: ok CONNECT')/$(guest_lines 'WS-CIC: reject CONNECT')" = 1/1
    check "an accepting CONNECT that closed gets exactly one DISCONNECT, after CONNECT returned (WS-CIC: ok DISCONNECT nested=0)" \
        test "$(guest_lines 'WS-CIC: ok DISCONNECT nested=0')" = 1
    check "never nested inside CONNECT (WS-CIC: ok DISCONNECT nested=1 x0)" \
        test "$(guest_lines 'WS-CIC: ok DISCONNECT nested=1')" = 0
    check "after closing inside CONNECT the connection is closed to the app: a send and a second close both fail (WS-CIC: ok after-close send=-1 close=-1)" \
        test "$(guest_lines 'WS-CIC: ok after-close send=-1 close=-1')" = 1
    check "a rejecting CONNECT gets no DISCONNECT (WS-CIC: reject DISCONNECT x0)" \
        test "$(guest_lines 'WS-CIC: reject DISCONNECT')" = 0
    check "counts at /stop (WS-CIC: counts ok=1/1 reject=1/0)" \
        test "$(guest_lines 'WS-CIC: counts ok=1/1 reject=1/0')" = 1
    check "boot 1: the heap recorded no corruption (WS-CIC: MEM-CORRUPTION 0)" \
        test "$(guest_lines 'WS-CIC: MEM-CORRUPTION 0')" = 1
    check "boot 1: the fixture ran to the end (WS-CIC: DONE)" \
        test "$(guest_lines 'WS-CIC: DONE')" = 1
    check "boot 1: no leaks at teardown" test_check_leaks
fi
end_boot

# ---- boot 2: an upload torn down after its clean end ----------------------
UP_BYTES=20000
HELD_OK='async send: completion held (injected by axl_tcp_fail_next_send_schedule), status 0'
HELD_ANY='async send: completion held (injected by axl_tcp_fail_next_send_schedule)'
HELD_REAL='async send: cannot schedule the completion callback'
if boot serve-upload-held; then
    UP_OUT=$(head -c "$UP_BYTES" /dev/zero | tr '\0' 'U' | \
             curl -s -X PUT --data-binary @- -H "Connection: close" \
                  -w ' %{http_code}' --max-time 15 \
                  "http://127.0.0.1:${HOST_PORT}/up" 2>/dev/null || true)
    sleep 1
    STOP_BODY=$(stop)
    test_wait_for "UP-HELD: DONE" 30 || true
    test_clean_log
    echo "  upload '$UP_OUT'; /stop '$STOP_BODY'; held(any) $(guest_count "$HELD_ANY")"
    grep -a '^UP-HELD:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

    check "CONTROL: the client got the handler's answer (stored 201)" \
        test "$UP_OUT" = "stored 201"
    check "CONTROL: the seam held exactly one completion, a finished one (status 0)" \
        test "$(guest_count "$HELD_OK")/$(guest_count "$HELD_ANY")" = 1/1
    check "CONTROL: no REAL both-pools-full hold happened" \
        test "$(guest_count "$HELD_REAL")" = 0
    check "CONTROL: before teardown the handler saw every byte and one clean EOF, no abort" \
        grep -aqE "^UP-HELD: before-free chunks=[1-9][0-9]* eof=1 abort=0 bytes=${UP_BYTES}\$" "$TEST_CLEAN_LOG"
    check "teardown after a clean end calls the handler no more (UP-HELD: after-free ... eof=1 abort=0)" \
        grep -aqE '^UP-HELD: after-free chunks=[1-9][0-9]* eof=1 abort=0$' "$TEST_CLEAN_LOG"
    check "boot 2: the heap recorded no corruption (UP-HELD: MEM-CORRUPTION 0)" \
        test "$(guest_lines 'UP-HELD: MEM-CORRUPTION 0')" = 1
    check "boot 2: the fixture ran to the end (UP-HELD: DONE)" \
        test "$(guest_lines 'UP-HELD: DONE')" = 1
    check "boot 2: no leaks at teardown" test_check_leaks
fi
end_boot

# ---- boot 3: an upload whose handler failed a chunk -----------------------
# A chunk call returning AXL_ERR ends the upload just as clean EOF does; the
# 507 it set goes out, its completion is held, and teardown must not add an
# abort call. (20000 bytes is one chunk, so this is the final-flush branch.)
if boot "serve-upload-held fail"; then
    UP_CODE=$(head -c "$UP_BYTES" /dev/zero | tr '\0' 'U' | \
              curl -s -o /dev/null -X PUT --data-binary @- \
                   -H "Connection: close" -w '%{http_code}' --max-time 15 \
                   "http://127.0.0.1:${HOST_PORT}/up" 2>/dev/null || true)
    sleep 1
    STOP_BODY=$(stop)
    test_wait_for "UP-HELD: DONE" 30 || true
    test_clean_log
    echo "  failed upload -> HTTP '$UP_CODE'; /stop '$STOP_BODY'; held(any) $(guest_count "$HELD_ANY")"
    grep -a '^UP-HELD:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

    check "CONTROL: the client got the handler's status (507)" \
        test "$UP_CODE" = 507
    check "CONTROL: boot 3's seam held exactly one completion, a finished one (status 0)" \
        test "$(guest_count "$HELD_OK")/$(guest_count "$HELD_ANY")" = 1/1
    check "CONTROL: before teardown the handler saw its failing chunk and nothing after it" \
        test "$(guest_lines "UP-HELD: before-free chunks=1 eof=0 abort=0 bytes=${UP_BYTES}")" = 1
    check "teardown after a chunk call that failed calls the handler no more (UP-HELD: after-free chunks=1 eof=0 abort=0)" \
        test "$(guest_lines 'UP-HELD: after-free chunks=1 eof=0 abort=0')" = 1
    check "boot 3: the heap recorded no corruption (UP-HELD: MEM-CORRUPTION 0)" \
        test "$(guest_lines 'UP-HELD: MEM-CORRUPTION 0')" = 1
    check "boot 3: no leaks at teardown" test_check_leaks
fi
end_boot

# ---- boot 4: a multi-chunk upload whose handler failed its first chunk ----
# The chunk-flush branch: with more of the body still arriving, the server
# must discard it -- no further chunk call, no clean-EOF call, no second
# response -- and teardown must add no abort call. (upload.chunk.size is
# 65536 by default, so 200000 bytes is three full chunks and a tail.)
BIG_BYTES=200000
if boot "serve-upload-held fail"; then
    BIG_CODE=$(head -c "$BIG_BYTES" /dev/zero | tr '\0' 'U' | \
               curl -s -o /dev/null -X PUT --data-binary @- \
                    -H "Connection: close" -w '%{http_code}' --max-time 15 \
                    "http://127.0.0.1:${HOST_PORT}/up" 2>/dev/null || true)
    sleep 1
    STOP_BODY=$(stop)
    test_wait_for "UP-HELD: DONE" 30 || true
    test_clean_log
    echo "  failed multi-chunk upload -> HTTP '$BIG_CODE'; /stop '$STOP_BODY'; held(any) $(guest_count "$HELD_ANY")"
    grep -a '^UP-HELD:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

    check "CONTROL: boot 4's seam held exactly one completion, a finished one (status 0)" \
        test "$(guest_count "$HELD_OK")/$(guest_count "$HELD_ANY")" = 1/1
    check "the handler got its failing first chunk and nothing after it, though the body went on (UP-HELD: before-free chunks=1 eof=0 abort=0 bytes=65536)" \
        test "$(guest_lines 'UP-HELD: before-free chunks=1 eof=0 abort=0 bytes=65536')" = 1
    check "teardown adds no call either (UP-HELD: after-free chunks=1 eof=0 abort=0)" \
        test "$(guest_lines 'UP-HELD: after-free chunks=1 eof=0 abort=0')" = 1
    check "boot 4: the heap recorded no corruption (UP-HELD: MEM-CORRUPTION 0)" \
        test "$(guest_lines 'UP-HELD: MEM-CORRUPTION 0')" = 1
    check "boot 4: no leaks at teardown" test_check_leaks
fi

echo ""
printf "HTTP teardown hooks: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial (last boot) ---"; tail -40 "$TEST_CLEAN_LOG"
fi
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
