#!/bin/bash
# test-meta: arch=x64 needs= est=18 local-only=0
# AxlTls HTTPS integration test — boots QEMU with TLS-enabled HTTP server,
# validates with curl --insecure from the host.
#
# Requires: AXL_TLS=1 build
# Usage: ./test/integration/test-https.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8443

# Build with TLS
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" AXL_TLS=1 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Startup: init network, start HTTPS server
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

echo Starting HTTPS server...
AxlTestNet.efi serve-tls
NSHEOF

test_build_image

echo "=== AxlTls HTTPS Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTPS server..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: HTTPS server did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"
    tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Server is ready"
sleep 2

# ---------------------------------------------------------------------------
# HTTPS tests via curl (--insecure for self-signed cert)
# ---------------------------------------------------------------------------

PASS=0
FAIL=0
BASE_URL="https://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s --insecure -H "Connection: close" --max-time 10)

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# GET /api/version over HTTPS
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "HTTPS GET /api/version returns 200" || fail "HTTPS GET (got $CODE)"
echo "$BODY" | grep -q '"version"' && pass "HTTPS response has version" || fail "HTTPS missing version"

# GET /plain over HTTPS
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "HTTPS GET /plain returns 200" || fail "HTTPS /plain (got $CODE)"
echo "$BODY" | grep -q "Hello from AxlNet" && pass "HTTPS /plain body correct" || fail "HTTPS /plain wrong body"

# Verify TLS is actually being used (cert info)
CERT_INFO=$(curl "${CURL_OPTS[@]}" -v "${BASE_URL}/plain" 2>&1 | grep -i 'SSL\|TLS\|subject' | head -3)
if echo "$CERT_INFO" | grep -qi 'TLS'; then
    pass "TLS handshake confirmed"
else
    fail "TLS handshake not detected"
fi

# ---------------------------------------------------------------------------
# RAPID RECONNECT: N back-to-back handshakes, no delay between them.
#
# Two independent sessions reported curl exiting 35 (SSL connect error) and 56
# on a request issued immediately after a previous one to an AXL TLS server —
# one inside SoftBMC's test.sh, one against a stock build. Neither could show
# it on demand, so this exists to turn "I saw it once" into a reproducer or
# rule the theory out.
#
# Every request already carries `Connection: close`, so each iteration is a
# FRESH TCP connection and a FRESH handshake — which is the scenario. The four
# requests above are the same shape and pass, so if this is rate-dependent it
# needs depth, not variety.
#
# Reported as a count rather than pass/fail-per-iteration: one flake in 40 is
# the finding, and 40 separate assertions would drown it.
# ---------------------------------------------------------------------------
# Overridable so a suspected flake can be chased at depth without changing
# what CI pays for every run: AXL_TLS_RECONNECT_N=500 ./test-https.sh
RECONNECT_N=${AXL_TLS_RECONNECT_N:-40}
rc_fail=0
declare -A rc_seen=()
for _i in $(seq 1 $RECONNECT_N); do
    curl -s --insecure -H "Connection: close" --max-time 5 \
         -o /dev/null "${BASE_URL}/plain"
    rc=$?
    if [[ $rc -ne 0 ]]; then
        rc_fail=$((rc_fail + 1))
        rc_seen[$rc]=$(( ${rc_seen[$rc]:-0} + 1 ))
    fi
done
if [[ $rc_fail -eq 0 ]]; then
    pass "$RECONNECT_N back-to-back TLS handshakes, no failures"
else
    detail=""
    for k in "${!rc_seen[@]}"; do detail+=" curl($k)x${rc_seen[$k]}"; done
    fail "TLS rapid reconnect: $rc_fail/$RECONNECT_N failed —$detail"
fi

# ---------------------------------------------------------------------------
# WebSocket over TLS (wss): inbound client->server frames must reach the
# handler. Regression for the TLS drain mis-routing decrypted WS frames into
# header_buf (select_decrypt_buf checked !headers_done before is_websocket).
# ---------------------------------------------------------------------------
while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
    esac
done < <(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, ssl, base64, os, time

port = int(sys.argv[1])
try:
    ctx = ssl._create_unverified_context()
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    s = ctx.wrap_socket(raw, server_hostname="127.0.0.1")
except Exception as e:
    print(f"FAIL: wss connect ({e})")
    sys.exit(0)

key = base64.b64encode(os.urandom(16)).decode()
req = ("GET /ws-echo-ex HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk
status = resp.split(b"\r\n")[0].decode()
if "101" not in status:
    print(f"FAIL: wss handshake (got {status!r})")
    sys.exit(0)
print("PASS: wss handshake 101")

# Send a masked text frame; the per-client handler must echo "ex:hello".
msg = b"hello"
frame = bytearray([0x81, 0x80 | len(msg)])
mask = os.urandom(4)
frame += mask + bytes(c ^ mask[i % 4] for i, c in enumerate(msg))
s.sendall(frame)

time.sleep(1)
try:
    data = s.recv(1024)
except Exception as e:
    data = b""
    print(f"FAIL: wss recv ({e})")
if len(data) >= 2:
    plen = data[1] & 0x7F
    payload = data[2:2 + plen]
    if payload == b"ex:hello":
        print("PASS: wss inbound frame delivered (echo 'ex:hello')")
    else:
        print(f"FAIL: wss echo wrong ({payload!r})")
else:
    print("FAIL: wss inbound frame dropped (no echo)")
s.close()
PYEOF
)

# ---------------------------------------------------------------------------
# Concurrent handshakes on one loop.
#
# Regression for AXL_BUSY being collapsed into a fatal error: when two
# handshakes interleave on a shared AxlLoop, a recv for a connection can be
# dispatched while that same socket's send completion is still queued, so the
# next flight hits axl_tcp_send_async with send_source still set. That returns
# AXL_BUSY -- retryable, bytes still buffered -- which handshake_flush_async
# turned into AXL_TLS_ERR, and the server killed a healthy connection.
#
# Sequential HTTPS cannot observe this at any request count: one connection at
# a time never contends the loop, so the "lock-step flights" assumption holds
# and the prior send has always drained. The requests below MUST be backgrounded
# and overlapping -- serialising them makes this check vacuous.
# ---------------------------------------------------------------------------
CONC_ROUNDS=6
CONC_WIDTH=4
conc_total=0
conc_ok=0
for _round in $(seq 1 $CONC_ROUNDS); do
    conc_codes=$(for _i in $(seq 1 $CONC_WIDTH); do
        # -w always emits exactly one line (000 on connect/handshake
        # failure), so no `|| echo` fallback -- that double-counts a
        # failing request and inflates the total.
        ( curl "${CURL_OPTS[@]}" -o /dev/null -w '%{http_code}\n' \
              "${BASE_URL}/plain" 2>/dev/null || true ) &
    done; wait)
    while IFS= read -r _code; do
        conc_total=$((conc_total + 1))
        [[ "$_code" == "200" ]] && conc_ok=$((conc_ok + 1))
    done <<< "$conc_codes"
done

if [[ $conc_total -ne $((CONC_ROUNDS * CONC_WIDTH)) ]]; then
    fail "concurrent HTTPS: harness collected $conc_total/$((CONC_ROUNDS * CONC_WIDTH)) results"
elif [[ $conc_ok -eq $conc_total ]]; then
    pass "concurrent HTTPS handshakes: $conc_ok/$conc_total OK (width $CONC_WIDTH)"
else
    fail "concurrent HTTPS handshakes: only $conc_ok/$conc_total OK (width $CONC_WIDTH)"
fi

echo ""
printf "HTTPS tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    test_clean_log
    echo "--- Serial log ---"
    tail -30 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
