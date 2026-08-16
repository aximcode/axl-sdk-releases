#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-socket-rebind-load-qemu.sh — axl_socket_free(RESET) parity under load.
#
# The BSD-style AxlSocket API is a veneer over AxlTcp. This proves
# axl_socket_free(RESET) gives a stream-socket listener the same port-releasing
# teardown as the HTTP path: with a firmware accept backlog at teardown, an
# immediate same-process re-listen on the port succeeds with NO pump. Same
# reproduction as test-http-rebind-load-qemu.sh, via the socket API.
#
# AxlTestNet.efi socket-rebind-load <graceful|abortive>:
#   abortive (GREEN): REBIND-RC == 0 + no leaked sources at loop_free.
#   graceful (RED): REBIND-RC != 0 — the backlog still holds the port.
#
# REBIND_VARIANT=graceful runs the RED comparison. BACKLOG_N (default 6).
#
# Usage: ./test/integration/test-socket-rebind-load-qemu.sh [--arch X64|AARCH64]

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

echo Starting socket-rebind-load (${VARIANT})...
AxlTestNet.efi socket-rebind-load ${VARIANT}
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== AxlSocket port-release under load ($TEST_ARCH, variant=$VARIANT, N=$BACKLOG_N) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "READY" 60; then
    echo "FAIL: socket did not reach READY within 60 seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# On READY: open N connections into the (unpumped) listener's accept backlog.
python3 - "$HOST_PORT" "$BACKLOG_N" << 'PYEOF' &
import socket, sys, time
port = int(sys.argv[1]); n = int(sys.argv[2])
socks = []
for _ in range(n):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /plain HTTP/1.1\r\nHost: x\r\n")
        socks.append(s)
    except Exception:
        pass
time.sleep(9)
for s in socks:
    try: s.close()
    except Exception: pass
PYEOF
LOAD_PID=$!

if ! test_wait_for "REBIND-RC:" 60; then
    echo "FAIL: no REBIND-RC within 60s"; kill "$LOAD_PID" 2>/dev/null
    test_clean_log; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"; exit 1
fi
test_wait_for "REBIND-DONE" 20 || true
kill "$LOAD_PID" 2>/dev/null

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
RC=$(grep -oE 'REBIND-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1)

if [[ "$VARIANT" == "abortive" ]]; then
    [[ "$RC" == "REBIND-RC:0" ]] \
        && pass "axl_socket_free(RESET) released :8080 under a $BACKLOG_N-deep backlog ($RC)" \
        || fail "socket abortive port-release ($RC — backlog still holds the port)"
    grep -q "caller-owned event source .* still active" "$TEST_CLEAN_LOG" \
        && fail "socket abortive teardown left a source active at loop_free" \
        || pass "no still-active caller-owned source at loop_free"
else
    [[ "$RC" != "REBIND-RC:0" ]] \
        && pass "axl_socket_free (graceful) leaves :8080 held under load ($RC) — expected" \
        || fail "graceful socket teardown unexpectedly released the port ($RC)"
fi

echo ""
printf "AxlSocket port-release under load: %d passed, %d failed (%s, %s)\n" "$PASS" "$FAIL" "$TEST_ARCH" "$VARIANT"
if [[ $FAIL -gt 0 ]]; then echo ""; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"; fi
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
