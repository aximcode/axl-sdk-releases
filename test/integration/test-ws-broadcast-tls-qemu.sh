#!/bin/bash
# test-ws-broadcast-tls-qemu.sh — rapid WS broadcast over TLS must not desync.
#
# Regression for the ws-broadcast-over-TLS stream-desync wedge (SoftBMC
# console-mirror): axl_tls_write_async advances the TLS write seqno at encrypt
# time, and axl_tcp_send_async is one-send-in-flight — so a burst of broadcasts
# racing one async completion used to encrypt-then-DROP the later frames,
# desyncing the TLS stream and wedging the driver-tick server loop. The mirror
# echoes one keystroke as >=3 back-to-back broadcasts, hitting this on the first
# key. The fix is a per-connection outbound queue that serializes frames
# (enqueue pre-encryption; one in flight at a time).
#
# AxlTestNet.efi serve-tls-ws-driver hosts an HTTPS server with a /ws-burst
# endpoint that fires WS_BURST_N (8) back-to-back broadcasts on any client
# frame. The harness connects a wss client, sends one frame, then asserts:
#   (a) all 8 frames arrive in order, byte-exact (intact TLS stream), and
#   (b) a fresh HTTPS GET still returns 200 (loop not wedged).
#
# Requires: AXL_TLS=1 build. Usage: ./test/integration/test-ws-broadcast-tls-qemu.sh

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=18447
GUEST_PORT=8443

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" AXL_TLS=1 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

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

echo Starting HTTPS+WS server (resident driver-tick loop)...
AxlTestNet.efi serve-tls-ws-driver
NSHEOF

test_build_image

echo "=== AxlTls WS-broadcast-over-TLS Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTPS+WS server..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Server is ready"
sleep 2

PASS=0
FAIL=0
BASE_URL="https://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s --insecure -H "Connection: close" --max-time 10)
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Step 1 — baseline GET.
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET /api/version returns 200 (pre-burst baseline)" \
                       || fail "HTTPS GET baseline (got '$CODE')"

# Step 2 — wss client: trigger the burst, read all 8 frames, verify byte-exact + order.
WS_RESULT=$(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, ssl, hashlib, base64, os, time

port = int(sys.argv[1])
N = 8

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
raw = socket.create_connection(("127.0.0.1", port), timeout=10)
s = ctx.wrap_socket(raw, server_hostname="127.0.0.1")
s.settimeout(10)

key = base64.b64encode(os.urandom(16)).decode()
req = ("GET /ws-burst HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk
if b"101" not in resp.split(b"\r\n")[0]:
    print("HANDSHAKE_FAIL")
    sys.exit(0)
# Any bytes after the header are the first WS frame(s) — keep them.
buf = resp.split(b"\r\n\r\n", 1)[1]

def send_text(msg):
    data = msg.encode()
    frame = bytearray([0x81, 0x80 | len(data)])
    mask = os.urandom(4)
    frame += mask
    frame += bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    s.sendall(frame)

# Minimal server->client (unmasked) text-frame reader over the TLS stream.
def read_frame():
    global buf
    def need(n):
        global buf
        while len(buf) < n:
            chunk = s.recv(4096)
            if not chunk:
                raise EOFError("connection closed")
            buf += chunk
    need(2)
    b0, b1 = buf[0], buf[1]
    opcode = b0 & 0x0F
    masked = b1 & 0x80
    plen = b1 & 0x7F
    hdr = 2
    if plen == 126:
        need(4); plen = int.from_bytes(buf[2:4], "big"); hdr = 4
    elif plen == 127:
        need(10); plen = int.from_bytes(buf[2:10], "big"); hdr = 10
    if masked:
        hdr += 4
    need(hdr + plen)
    payload = buf[hdr:hdr + plen]
    buf = buf[hdr + plen:]
    return opcode, bytes(payload)

# Trigger the burst.
send_text("go")

# Read N frames; they must be WSBURST0..WSBURST{N-1} in order, byte-exact.
got = []
try:
    while len(got) < N:
        op, pl = read_frame()
        if op == 0x1:                 # text
            got.append(pl.decode("latin-1"))
        elif op == 0x8:               # close
            break
except Exception as e:
    print(f"RECV_ERROR {type(e).__name__}")

expected = [f"WSBURST{i}" for i in range(N)]
if got == expected:
    print(f"BURST_OK {len(got)}")
else:
    print(f"BURST_MISMATCH got={got!r}")
try:
    s.close()
except Exception:
    pass
PYEOF
)
echo "  WS client result: $WS_RESULT"
echo "$WS_RESULT" | grep -q "BURST_OK 8" \
    && pass "all 8 TLS WS broadcasts arrived in order, byte-exact (no desync)" \
    || fail "TLS WS broadcast burst (got '$WS_RESULT')"

# Step 3 — THE wedge check: the loop must still serve after the burst.
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET after burst returns 200 (loop not wedged)" \
                       || fail "HTTPS GET after burst (got '$CODE' — loop wedged)"

echo ""
printf "WS-broadcast-over-TLS tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; test_clean_log; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
