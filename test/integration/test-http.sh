#!/bin/bash
# AxlNet HTTP server integration test — boots QEMU with port forwarding,
# starts the HTTP server inside UEFI, validates with curl from the host.
#
# Usage: ./test/integration/test-http.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=18080
GUEST_PORT=8080

# Stage test app
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Startup script: init network, run DHCP, start HTTP server
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

echo Network status:
ifconfig -l
stall 1000000

echo Starting HTTP server...
AxlTestNet.efi serve
NSHEOF

test_build_image

echo "=== AxlNet HTTP Integration Test ($TEST_ARCH) ==="

# ---------------------------------------------------------------------------
# Start a Python HTTP server on the host for UEFI client tests.
# The UEFI guest reaches it via QEMU gateway at 10.0.2.2.
# ---------------------------------------------------------------------------

HOST_SERVER_PORT=18081
HOST_SERVER_PID=0

python3 "$(dirname "$0")/host-server.py" "$HOST_SERVER_PORT" &
HOST_SERVER_PID=$!

trap 'test_cleanup; [[ $HOST_SERVER_PID -gt 0 ]] && kill $HOST_SERVER_PID 2>/dev/null || true' EXIT

sleep 1
echo "  Host HTTP server PID: $HOST_SERVER_PID, port: $HOST_SERVER_PORT"

# Boot QEMU with networking and port forwarding
test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTP server..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: HTTP server did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"
    tail -20 "$TEST_CLEAN_LOG"
    echo "---"
    exit 1
fi

echo "  Server is ready"
sleep 2

# ---------------------------------------------------------------------------
# HTTP tests via curl
# ---------------------------------------------------------------------------

PASS=0
FAIL=0
BASE_URL="http://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s -H "Connection: close" --max-time 10)

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

http_get() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}$1" 2>/dev/null || true
}

http_post() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "$2" "${BASE_URL}$1" 2>/dev/null || true
}

echo ""
echo "  --- HTTP Server Tests ---"

# GET /api/version — JSON response
RESP=$(http_get "/api/version")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /api/version returns 200" || fail "GET /api/version (got $CODE)"
echo "$BODY" | grep -q '"version"' && pass "/api/version body has version" || fail "/api/version missing version"

# GET /api/health — JSON health check
RESP=$(http_get "/api/health")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /api/health returns 200" || fail "GET /api/health (got $CODE)"
echo "$BODY" | grep -q '"status"' && pass "/api/health body has status" || fail "/api/health missing status"

# GET /plain — text response
RESP=$(http_get "/plain")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /plain returns 200" || fail "GET /plain (got $CODE)"
echo "$BODY" | grep -q "Hello from AxlNet" && pass "/plain has expected text" || fail "/plain wrong body"

# POST /echo — echo body back
RESP=$(http_post "/echo" "test payload")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /echo returns 200" || fail "POST /echo (got $CODE)"
echo "$BODY" | grep -q "test payload" && pass "/echo returns posted body" || fail "/echo wrong body"

# GET /nonexistent — 404
RESP=$(http_get "/nonexistent")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "404" ]] && pass "GET /nonexistent returns 404" || fail "GET /nonexistent (got $CODE)"

# ---------------------------------------------------------------------------
# Route lookup: exact vs prefix
#
# /rt/         and /rt/*         are both registered. Same for
# /rt/files    and /rt/files/*.  Verify the exact route always wins at
# its own path and the prefix takes over at any sub-path. This
# regression-tests the split-tree fix where exact and prefix routes
# previously collided on the same radix key.
# ---------------------------------------------------------------------------

# Exact / prefix at the same level
RESP=$(http_get "/rt/")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/ returns 200" || fail "GET /rt/ (got $CODE)"
[[ "$BODY" == "exact" ]] && pass "GET /rt/ matches exact route" || fail "GET /rt/ wrong handler ($BODY)"

RESP=$(http_get "/rt/anything")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/anything returns 200" || fail "GET /rt/anything (got $CODE)"
[[ "$BODY" == "prefix" ]] && pass "GET /rt/anything matches prefix route" || fail "GET /rt/anything wrong handler ($BODY)"

RESP=$(http_get "/rt/a/b/c")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/a/b/c returns 200" || fail "GET /rt/a/b/c (got $CODE)"
[[ "$BODY" == "prefix" ]] && pass "GET /rt/a/b/c matches prefix route" || fail "GET /rt/a/b/c wrong handler ($BODY)"

# Nested exact / prefix — longest-prefix wins, exact still beats it at the same path
RESP=$(http_get "/rt/files")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/files returns 200" || fail "GET /rt/files (got $CODE)"
[[ "$BODY" == "nested-exact" ]] && pass "GET /rt/files matches nested exact" || fail "GET /rt/files wrong handler ($BODY)"

RESP=$(http_get "/rt/files/foo.txt")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/files/foo.txt returns 200" || fail "GET /rt/files/foo.txt (got $CODE)"
[[ "$BODY" == "nested-prefix" ]] && pass "GET /rt/files/foo.txt matches nested prefix" || fail "GET /rt/files/foo.txt wrong handler ($BODY)"

# Content-Type header check
CT=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{content_type}" "${BASE_URL}/api/version" 2>/dev/null || true)
echo "$CT" | grep -qi "application/json" && pass "Content-Type is application/json" || fail "Content-Type is '$CT'"

# ---------------------------------------------------------------------------
# Authentication tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- Authentication Tests ---"

# GET /secret without credentials → 401
RESP=$(http_get "/secret")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "401" ]] && pass "GET /secret no auth returns 401" || fail "GET /secret no auth (got $CODE)"

# GET /secret with wrong token → 401
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -H "Authorization: Bearer wrong" "${BASE_URL}/secret" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "401" ]] && pass "GET /secret wrong token returns 401" || fail "GET /secret wrong token (got $CODE)"

# GET /secret with valid token → 200
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -H "Authorization: Bearer test-token" "${BASE_URL}/secret" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /secret valid token returns 200" || fail "GET /secret valid token (got $CODE)"
echo "$BODY" | grep -q '"secret"' && pass "/secret has expected body" || fail "/secret wrong body"

# ---------------------------------------------------------------------------
# Response caching tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- Response Caching Tests ---"

# First request — handler runs, count=1
RESP=$(http_get "/cached")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /cached first request returns 200" || fail "GET /cached first (got $CODE)"
echo "$BODY" | grep -q '"count":1' && pass "/cached first request count=1" || fail "/cached first count wrong ($BODY)"

# Second request — should be cached, still count=1
RESP=$(http_get "/cached")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /cached second request returns 200" || fail "GET /cached second (got $CODE)"
echo "$BODY" | grep -q '"count":1' && pass "/cached second request still count=1 (cached)" || fail "/cached second count changed ($BODY)"

# ---------------------------------------------------------------------------
# cache_max FIFO eviction — server is configured with use_cache(s, 3).
# Walk four distinct paths /cm/1../cm/4; after /cm/4 the oldest entry
# (/cm/1) must have been evicted, and a fresh GET /cm/1 should re-run
# the handler (count incremented).
# ---------------------------------------------------------------------------

echo ""
echo "  --- Cache Eviction Tests (cache_max) ---"

RESP=$(http_get "/cm/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/1 first request count=1" || fail "/cm/1 first count wrong ($BODY)"

RESP=$(http_get "/cm/2")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/2 first request count=1" || fail "/cm/2 first count wrong ($BODY)"

RESP=$(http_get "/cm/3")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/3 first request count=1" || fail "/cm/3 first count wrong ($BODY)"

# cm/1 should still be in cache at this point (size 3 = max, nothing forced eviction yet)
RESP=$(http_get "/cm/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/1 still cached (count=1)" || fail "/cm/1 unexpectedly re-ran ($BODY)"

# /cm/4 pushes cache over limit — oldest (/cm/1) is evicted
RESP=$(http_get "/cm/4")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/4 first request count=1" || fail "/cm/4 first count wrong ($BODY)"

# /cm/1 should now be a miss — handler re-runs
RESP=$(http_get "/cm/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/cm/1 evicted and re-run (count=2)" || fail "/cm/1 eviction failed ($BODY)"

# ---------------------------------------------------------------------------
# Per-route TTL — /ttl-short has a 1.5s TTL, /ttl-long uses the
# server-wide 60-second default. After sleeping 2s, /ttl-short
# must expire (handler re-runs) while /ttl-long stays cached.
# (TTL must exceed UEFI's 1-second clock granularity so that
# `now - timestamp_ms` is non-zero on the second request.)
# ---------------------------------------------------------------------------

echo ""
echo "  --- Per-route TTL Tests ---"

RESP=$(http_get "/ttl-short")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/ttl-short first request count=1" || fail "/ttl-short first count wrong ($BODY)"

RESP=$(http_get "/ttl-long")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/ttl-long first request count=1" || fail "/ttl-long first count wrong ($BODY)"

sleep 2

RESP=$(http_get "/ttl-short")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/ttl-short expired and re-run (count=2)" || fail "/ttl-short TTL not honored ($BODY)"

RESP=$(http_get "/ttl-long")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/ttl-long still cached (count=1)" || fail "/ttl-long unexpectedly re-ran ($BODY)"

# ---------------------------------------------------------------------------
# Prefix-based cache invalidation — after caching three entries under
# /api/users/* and /api/posts/1, GET /invalidate-users calls
# axl_http_server_cache_invalidate(s, "/api/users"), which must
# remove both /api/users/* entries and leave /api/posts/1 intact.
# ---------------------------------------------------------------------------

echo ""
echo "  --- Prefix Cache Invalidation Tests ---"

RESP=$(http_get "/api/users/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/users/1 first request count=1" || fail "/api/users/1 first count wrong ($BODY)"

RESP=$(http_get "/api/users/2")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/users/2 first request count=1" || fail "/api/users/2 first count wrong ($BODY)"

RESP=$(http_get "/api/posts/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/posts/1 first request count=1" || fail "/api/posts/1 first count wrong ($BODY)"

# Sanity: /api/users/1 should still be cached
RESP=$(http_get "/api/users/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/users/1 cached before invalidate" || fail "/api/users/1 not cached ($BODY)"

# Trigger invalidate
RESP=$(http_get "/invalidate-users")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "204" ]] && pass "/invalidate-users returns 204" || fail "/invalidate-users wrong code ($CODE)"

# /api/users/* should now miss and re-run
RESP=$(http_get "/api/users/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/api/users/1 invalidated (count=2)" || fail "/api/users/1 still cached ($BODY)"

RESP=$(http_get "/api/users/2")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/api/users/2 invalidated (count=2)" || fail "/api/users/2 still cached ($BODY)"

# /api/posts/1 should still be cached (prefix didn't match)
RESP=$(http_get "/api/posts/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/posts/1 still cached after invalidate" || fail "/api/posts/1 wrongly invalidated ($BODY)"

# ---------------------------------------------------------------------------
# Upload streaming tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- Upload Streaming Tests ---"

# Small upload (fits in one chunk with 1024 chunk size)
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "hello" "${BASE_URL}/upload" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /upload small returns 200" || fail "POST /upload small (got $CODE)"
echo "$BODY" | grep -q '"chunks":1' && pass "/upload small: 1 chunk" || fail "/upload small chunks (body: $BODY)"
echo "$BODY" | grep -q '"total":5' && pass "/upload small: 5 bytes total" || fail "/upload small total (body: $BODY)"

# Large upload (4KB > 1024 chunk size, so multiple chunks expected)
LARGE_DATA=$(python3 -c "print('X' * 4096, end='')")
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "$LARGE_DATA" "${BASE_URL}/upload" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /upload large returns 200" || fail "POST /upload large (got $CODE)"
echo "$BODY" | grep -qE '"chunks":[2-9]' && pass "/upload large: multiple chunks" || fail "/upload large chunks (body: $BODY)"
echo "$BODY" | grep -q '"total":4096' && pass "/upload large: 4096 bytes total" || fail "/upload large total (body: $BODY)"

# ---------------------------------------------------------------------------
# WebSocket tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- WebSocket Tests ---"

# WebSocket echo test using Python
while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
    esac
done < <(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, hashlib, base64, os, time

port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(("127.0.0.1", port))
except Exception as e:
    print(f"FAIL: ws connect ({e})")
    sys.exit(1)

# Handshake
key = base64.b64encode(os.urandom(16)).decode()
req = (
    "GET /ws-echo HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
)
s.sendall(req.encode())

resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk

status_line = resp.split(b"\r\n")[0].decode()
if "101" not in status_line:
    print(f"FAIL: ws handshake (got {status_line})")
    s.close()
    sys.exit(0)
print("PASS: ws handshake returns 101")

# Verify accept key
accept_input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
expected_accept = base64.b64encode(hashlib.sha1(accept_input.encode()).digest()).decode()
if expected_accept.encode() in resp:
    print("PASS: ws accept key correct")
else:
    print("FAIL: ws accept key mismatch")

# Send a masked text frame
msg = b"hello-ws"
frame = bytearray()
frame.append(0x81)  # FIN + TEXT
frame.append(0x80 | len(msg))  # MASK bit + length
mask_key = os.urandom(4)
frame.extend(mask_key)
for i, b in enumerate(msg):
    frame.append(b ^ mask_key[i % 4])
s.sendall(frame)

# Read echo response
time.sleep(1)
try:
    data = s.recv(1024)
    if len(data) >= 2:
        opcode = data[0] & 0x0F
        plen = data[1] & 0x7F
        payload = data[2:2+plen]
        if opcode == 1 and payload == msg:
            print("PASS: ws echo received correct data")
        else:
            print(f"FAIL: ws echo wrong (opcode={opcode}, payload={payload!r})")
    else:
        print("FAIL: ws echo no data")
except socket.timeout:
    print("FAIL: ws echo timeout")

# Close
time.sleep(0.5)
close_frame = bytearray([0x88, 0x80]) + os.urandom(4)
try:
    s.sendall(close_frame)
except:
    pass
s.close()
PYEOF
)

# ---------------------------------------------------------------------------
# UEFI HTTP Client tests (UEFI fetches from host Python server)
# ---------------------------------------------------------------------------

echo ""
echo "  --- UEFI HTTP Client Tests ---"

# Client GET — UEFI fetches /hello from host server via QEMU gateway
RESP=$(http_get "/client-test?url=http://10.0.2.2:${HOST_SERVER_PORT}/hello")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "client-test proxy returns 200" || fail "client-test proxy (got $CODE)"
echo "$BODY" | grep -q '"client_status":200' && pass "AxlHttpGet got 200 from host" || fail "AxlHttpGet wrong status (body: $BODY)"
echo "$BODY" | grep -q 'hello from host' && pass "AxlHttpGet got correct body" || fail "AxlHttpGet wrong body (body: $BODY)"

# Client GET — redirect (UEFI fetches /redirect which 302s to /hello)
RESP=$(http_get "/client-test?url=http://10.0.2.2:${HOST_SERVER_PORT}/redirect")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "client-test redirect returns 200" || fail "client-test redirect (got $CODE)"
echo "$BODY" | grep -q '"client_status":200' && pass "AxlHttpGet follows redirect" || fail "AxlHttpGet redirect failed (body: $BODY)"

# Client GET — 404
RESP=$(http_get "/client-test?url=http://10.0.2.2:${HOST_SERVER_PORT}/nonexistent")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "client-test 404 proxy returns 200" || fail "client-test 404 proxy (got $CODE)"
echo "$BODY" | grep -q '"client_status":404' && pass "AxlHttpGet sees 404 from host" || fail "AxlHttpGet didn't see 404 (body: $BODY)"

# Client GET — Transfer-Encoding: chunked. Host emits two body chunks
# (0xb + 0x9 = 0x14 = 20 bytes total) plus terminator. Decoded body must
# be the exact concatenation "hello-chunk-second!!".
RESP=$(http_get "/client-test?url=http://10.0.2.2:${HOST_SERVER_PORT}/chunked")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "client-test chunked proxy returns 200" || fail "client-test chunked proxy (got $CODE)"
echo "$BODY" | grep -q '"client_status":200' && pass "AxlHttpGet got 200 from chunked host" || fail "AxlHttpGet wrong status from chunked (body: $BODY)"
echo "$BODY" | grep -q '"client_body_size":20' && pass "AxlHttpGet decoded chunked body size=20" || fail "AxlHttpGet chunked body size (body: $BODY)"
echo "$BODY" | grep -q '"client_body":"hello-chunk-second!!"' && pass "AxlHttpGet decoded chunked body bytes" || fail "AxlHttpGet chunked body bytes (body: $BODY)"

# Client GET — chunked with chunk extensions (`b;name=foo`) and a
# trailer header (`X-Trace`). Real servers emit these in production.
RESP=$(http_get "/client-test?url=http://10.0.2.2:${HOST_SERVER_PORT}/chunked-ext")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "client-test chunked-ext proxy returns 200" || fail "client-test chunked-ext proxy (got $CODE)"
echo "$BODY" | grep -q '"client_body_size":20' && pass "AxlHttpGet ignores chunk extensions" || fail "AxlHttpGet chunked-ext body size (body: $BODY)"
echo "$BODY" | grep -q '"client_body":"hello-chunk-second!!"' && pass "AxlHttpGet decoded chunked-ext body bytes" || fail "AxlHttpGet chunked-ext body bytes (body: $BODY)"

# Client GET — both Content-Length and Transfer-Encoding: chunked.
# RFC 7230 §3.3.3: chunked wins, the bogus Content-Length is ignored.
RESP=$(http_get "/client-test?url=http://10.0.2.2:${HOST_SERVER_PORT}/chunked-with-cl")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "client-test chunked+CL proxy returns 200" || fail "client-test chunked+CL proxy (got $CODE)"
echo "$BODY" | grep -q '"client_body_size":20' && pass "AxlHttpGet prefers chunked over Content-Length" || fail "AxlHttpGet chunked+CL body size (body: $BODY)"
echo "$BODY" | grep -q '"client_body":"hello-chunk-second!!"' && pass "AxlHttpGet decoded chunked+CL body bytes" || fail "AxlHttpGet chunked+CL body bytes (body: $BODY)"

echo ""
echo "Results: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
