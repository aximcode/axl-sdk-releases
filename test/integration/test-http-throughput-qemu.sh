#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 guest=standard
# test-http-throughput-qemu.sh -- AxlTcp transmit pacing, seen through AxlHttp.
#
# A 1 MiB GET is ONE axl_tcp_send_async of the whole body. Two firmware limits
# below AxlTcp used to make it take 7-10 s (x64) and 7.2 s (aa64):
#   - EDK2 MnpDxe drains one received frame per 10 ms timer tick, and a TcpDxe
#     Transmit token completes only once its tail is acknowledged, so every
#     chunk waited ~10 ms per segment for its ACKs to be PROCESSED;
#   - iPXE's SNP (x64's e1000 ROM) refuses the 16th frame since the last poll
#     with EFI_OUT_OF_RESOURCES, which MnpDxe does not retry, so a burst that
#     deep lost a segment until TCP's RTO.
# AxlTcp now sends in 8 KB chunks, polls before every Transmit, and drives
# Tcp4->Poll() from the event loop while a send is pending.
#
# Checks, each GET with its own exact expectations:
#   1. three back-to-back GET /big: 200, exactly 1048576 bytes, the md5 of the
#      known pattern, and under THROUGHPUT_BOUND_SECS each;
#   1b. the pacing settings: out-of-range values refused with the defaults
#      kept (exact reply strings), and the piece size changed in BOTH
#      directions while a 1 MiB send is on the wire, every byte arriving;
#   1c. GET /plain answers while an 8 MiB transfer is actively progressing
#      (the pacer polling), and that transfer is byte-exact;
#   2. seven WebSocket clients, each broadcast frame going out as seven
#      back-to-back Transmits from one callback: every echo byte-exact and
#      every round under WS_ROUND_BOUND_SECS (a burst past iPXE's 15 frames
#      costs an RTO);
#   3. the loop stays responsive while a send is stalled at a zero window by
#      a client that stops reading: GET /plain answers "Hello from AxlNet" in
#      under RESPONSIVE_BOUND_SECS;
#   4. the stalled send does not hold a core (pacing backs off when no send
#      makes progress); it completes byte-exact once the client reads; and
#      after a client resets mid-transfer the server serves /big again;
#   5. once every connection is gone, the guest's CPU is under the budget
#      again (the pacing poll stops when nothing is pending).
#
# The bounds are DEBUG-build numbers (the suite builds DEBUG). Measured:
# before pacing 7-10 s (x64) / 7.2 s (aa64) per GET; after, see the commit.
#
# Usage: ./test/integration/test-http-throughput-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080
BIG_SIZE=1048576
# 0.75 s: the pre-pacing 7-10 s and a single ~1 s RTO (a 32 KB burst on
# iPXE) both fail it; paced DEBUG GETs measure 0.05-0.15 s.
THROUGHPUT_BOUND_SECS="0.75"
RESPONSIVE_BOUND_SECS="1.0"
WS_ROUND_BOUND_SECS="0.5"
# Host cores (utime+stime delta / wall). An idle guest measures a few
# hundredths; a guest spinning on Poll() holds ~1.0. 0.50 separates the two
# with a wide margin on both KVM and TCG.
CPU_BUDGET_CORES="0.50"
SAMPLE_SECS=4

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# The startup commands live in test-http-throughput-qemu.nsh
test_use_nsh

test_build_image

echo "=== AxlHttp 1 MiB throughput / AxlTcp transmit pacing ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

SLOW_PID=0
trap '[[ $SLOW_PID -gt 0 ]] && { pkill -P "$SLOW_PID" 2>/dev/null; kill "$SLOW_PID" 2>/dev/null; }; test_cleanup' EXIT

if ! test_wait_boot "READY" 90; then
    echo "FAIL: server did not reach READY within ${TEST_WAITED_S} seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 2

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

BASE_URL="http://127.0.0.1:${HOST_PORT}"
BODY="$TEST_TMPDIR/big.bin"
# The fixture's body: byte i = top byte of (i * 2654435761) mod 2^32 -- not
# periodic at any piece size, so skipped or repeated pieces change the md5.
EXPECTED_MD5=$(python3 -c 'import sys; sys.stdout.buffer.write(bytes(((i * 2654435761) & 0xffffffff) >> 24 for i in range(1 << 20)))' \
    | md5sum | cut -d' ' -f1)

HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
# The guest's QEMU: this test's own when booted; on a leased guest
# TEST_QEMU_PID is empty and the pool names the guest's (AXL_POOL_QEMU_PID).
GUEST_QEMU_PID="${TEST_QEMU_PID:-${AXL_POOL_QEMU_PID:-}}"
read_cpu() { awk '{n=index($0,") ");split(substr($0,n+2),f," ");print f[12]+f[13]}' \
    "/proc/${GUEST_QEMU_PID}/stat" 2>/dev/null || true; }
# sample_cores LABEL -> prints host cores used over SAMPLE_SECS, or "n/a"
sample_cores() {
    local c0 c1
    c0=$(read_cpu)
    sleep "$SAMPLE_SECS"
    c1=$(read_cpu)
    if [[ -z "$c0" || -z "$c1" ]]; then
        echo "n/a"
        return
    fi
    awk -v a="$c0" -v b="$c1" -v hz="$HZ" -v t="$SAMPLE_SECS" \
        'BEGIN { printf "%.3f", (b - a) / (hz * t) }'
}
under() { awk -v v="$1" -v b="$2" 'BEGIN { print (v + 0 < b + 0) ? 1 : 0 }'; }

# get_big LABEL -- one GET /big with every expectation exact.
get_big() {
    local label=$1 out code size secs md5
    rm -f "$BODY"
    out=$(curl -s -H "Connection: close" --max-time 60 -o "$BODY" \
               -w "%{http_code} %{size_download} %{time_total}" \
               "${BASE_URL}/big" 2>/dev/null || true)
    read -r code size secs <<< "$out"
    md5=$(md5sum "$BODY" 2>/dev/null | cut -d' ' -f1 || true)
    echo "  $label: code=${code:-none} bytes=${size:-0} secs=${secs:-?}"
    if [[ "$code" == "200" && "$size" == "$BIG_SIZE" && "$md5" == "$EXPECTED_MD5" ]]; then
        pass "$label: 200, $BIG_SIZE bytes, md5 $EXPECTED_MD5"
    else
        fail "$label: want 200/$BIG_SIZE/$EXPECTED_MD5, got ${code:-none}/${size:-0}/${md5:-none}"
    fi
    if [[ -n "$secs" && $(under "$secs" "$THROUGHPUT_BOUND_SECS") == 1 ]]; then
        pass "$label: ${secs}s < ${THROUGHPUT_BOUND_SECS}s"
    else
        fail "$label: ${secs:-?}s is not under ${THROUGHPUT_BOUND_SECS}s (transmit pacing)"
    fi
}

# --- 0. idle baseline, before any traffic ---
IDLE_BEFORE=$(sample_cores)
echo "  idle CPU before traffic: ${IDLE_BEFORE} cores"

# --- 1. back-to-back 1 MiB GETs ---
for r in 1 2 3; do
    get_big "GET /big #$r"
done

# --- 1b. the pacing settings, and changing them mid-send ---
# pacing_get QUERY -- GET /pacing?QUERY, print the one-line reply.
pacing_get() {
    curl -s -H "Connection: close" --max-time 10 "${BASE_URL}/pacing${1:+?$1}" 2>/dev/null || true
}
# expect_pacing QUERY WANT LABEL
expect_pacing() {
    local got
    got=$(pacing_get "$1")
    if [[ "$got" == "$2" ]]; then
        pass "$3: '$got'"
    else
        fail "$3: want '$2', got '${got}'"
    fi
}
expect_pacing "chunk=1023" "rc=INVALID chunk=8192 backoff=500" "pacing: chunk 1023 refused, defaults kept"
expect_pacing "backoff=10001" "rc=INVALID chunk=8192 backoff=500" "pacing: backoff 10001 refused, defaults kept"

# flip_mid_send -- start a 1 MiB GET under FROM, change to TO while it is on
# the wire, and require every byte. A completion that retired a length
# recomputed from the NEW setting instead of the ARMED one would skip bytes
# (upward) or resend them (downward). FROM uses multi-segment pieces: 1 KB
# pieces do not crawl even at back-off 0, because the poll before each
# Transmit drains a one-segment piece's ACK.
FLIP_BODY="$TEST_TMPDIR/flip.bin"
# The change is made FLIP_DELAY into the transfer. Back-off 0 must make the
# FROM transfer crawl (no loop-driven polling: multi-segment pieces wait on
# the firmware's 10 ms-per-frame drain, ~7 s per MiB), so the GET must still
# be running when the change is issued. That ordering is asserted,
# not assumed: it is what makes this a mid-send change at all, and it fails
# if back-off 0 were still polling (the GET would finish in ~0.1 s). Each
# starting setting also asks for latency=firmware: an axl_net_set_latency
# policy polls every 1 ms on its own, which drains a back-off-0 send far
# faster than the crawl this needs (measured 2026-11-03: the GET was done
# before the change) -- stated, so a change of the library's default cannot
# move it. The closing no-query /pacing restores both.
FLIP_DELAY=1.5
now_s() { date +%s.%N; }
# flip_mid_send FROM_QUERY WANT_FROM_REPLY TO_QUERY WANT_TO_REPLY LABEL
flip_mid_send() {
    local code size md5 reply from_reply t_issue t_end
    local from=$1 want_from=$2 to=$3 want_to=$4 label=$5
    from_reply=$(pacing_get "$from")
    if [[ "$from_reply" == "$want_from" ]]; then
        pass "$label: starting setting replied '$from_reply'"
    else
        fail "$label: starting setting: want '$want_from', got '${from_reply}'"
    fi
    rm -f "$FLIP_BODY" "$TEST_TMPDIR/flip.end"
    ( curl -s -H "Connection: close" --max-time 60 -o "$FLIP_BODY" \
           -w "%{http_code} %{size_download}" "${BASE_URL}/big" > "$TEST_TMPDIR/flip.out" 2>/dev/null
      now_s > "$TEST_TMPDIR/flip.end" ) &
    SLOW_PID=$!
    sleep "$FLIP_DELAY"
    # Stamped BEFORE the change is issued: the GET must still be running at
    # this instant. (After the change a 1 KB / back-off 500 remainder drains
    # in tens of ms, so comparing against the REPLY's arrival would measure
    # host scheduling, not the guest.)
    t_issue=$(now_s)
    reply=$(pacing_get "$to")
    wait "$SLOW_PID" 2>/dev/null || true
    SLOW_PID=0
    t_end=$(cat "$TEST_TMPDIR/flip.end" 2>/dev/null || echo 0)
    if [[ "$reply" == "$want_to" ]]; then
        pass "$label: mid-send set replied '$reply'"
    else
        fail "$label: mid-send set: want '$want_to', got '${reply}'"
    fi
    if [[ $(awk -v e="$t_end" -v r="$t_issue" 'BEGIN { print (e > r) ? 1 : 0 }') == 1 ]]; then
        pass "$label: the GET was still on the wire when the change was issued ($(awk -v e="$t_end" -v r="$t_issue" 'BEGIN { printf "%.2f", e - r }')s after)"
    else
        fail "$label: the GET finished before the change was issued (back-off 0 not crawling?) -- nothing mid-send was tested"
    fi
    code="" size=""
    read -r code size < "$TEST_TMPDIR/flip.out" || true
    md5=$(md5sum "$FLIP_BODY" 2>/dev/null | cut -d' ' -f1 || true)
    if [[ "$code" == "200" && "$size" == "$BIG_SIZE" && "$md5" == "$EXPECTED_MD5" ]]; then
        pass "$label: 200, $BIG_SIZE bytes, md5 $EXPECTED_MD5"
    else
        fail "$label: want 200/$BIG_SIZE/$EXPECTED_MD5, got ${code:-none}/${size:-0}/${md5:-none}"
    fi
}
flip_mid_send "chunk=16384&backoff=0&latency=firmware" "rc=OK chunk=16384 backoff=0" \
              "chunk=32768&backoff=500" "rc=OK chunk=32768 backoff=500" \
              "chunk raised mid-send (16 KB -> 32 KB)"
flip_mid_send "chunk=32768&backoff=0&latency=firmware" "rc=OK chunk=32768 backoff=0" \
              "chunk=1024&backoff=500" "rc=OK chunk=1024 backoff=500" \
              "chunk lowered mid-send (32 KB -> 1 KB)"
expect_pacing "" "rc=OK chunk=8192 backoff=500" "pacing: no query restores the defaults"

# --- 1c. the loop answers while a send is actively progressing ---
# A reader draining /big-stream at 2 MB/s keeps the guest's send making
# progress for ~4 s, so the pacer is polling the whole time.
curl -s -H "Connection: close" --limit-rate 2M --max-time 60 -o "$TEST_TMPDIR/stream.bin" \
     -w "%{http_code} %{size_download}" "${BASE_URL}/big-stream" > "$TEST_TMPDIR/stream.out" 2>/dev/null &
SLOW_PID=$!
sleep 1
out=$(curl -s -H "Connection: close" --max-time 10 -w "\n%{http_code} %{time_total}" \
           "${BASE_URL}/plain" 2>/dev/null || true)
PLAIN_BODY=$(head -1 <<< "$out")
read -r code secs <<< "$(tail -1 <<< "$out")"
echo "  GET /plain during an active 8 MiB transfer: code=${code:-none} secs=${secs:-?}"
if [[ "$code" == "200" && "$PLAIN_BODY" == "Hello from AxlNet" \
      && -n "$secs" && $(under "$secs" "$RESPONSIVE_BOUND_SECS") == 1 ]]; then
    pass "loop responsive while pacing: /plain 200 'Hello from AxlNet' in ${secs}s"
else
    fail "loop responsive while pacing: want 200 'Hello from AxlNet' under ${RESPONSIVE_BOUND_SECS}s, got ${code:-none} '${PLAIN_BODY}' in ${secs:-?}s"
fi
wait "$SLOW_PID" 2>/dev/null || true
SLOW_PID=0
STREAM_MD5=$(python3 -c 'import hashlib,sys; sys.stdout.write(hashlib.md5(bytes(((i * 2654435761) & 0xffffffff) >> 24 for i in range(1 << 20)) * 8).hexdigest())')
code="" size=""
read -r code size < "$TEST_TMPDIR/stream.out" || true
if [[ "$code" == "200" && "$size" == "8388608" \
      && "$(md5sum "$TEST_TMPDIR/stream.bin" | cut -d' ' -f1)" == "$STREAM_MD5" ]]; then
    pass "active 8 MiB transfer: 200, 8388608 bytes, md5 $STREAM_MD5"
else
    fail "active 8 MiB transfer: want 200/8388608/$STREAM_MD5, got ${code:-none}/${size:-0}"
fi

# --- 2. several connections transmitting in one loop turn ---
# /ws-echo broadcasts each text frame to every client from ONE callback: K
# back-to-back Transmits, one per connection. A 7000-byte frame is 5 segments
# at MSS 1460, so 7 clients put 35 frames on the NIC in one turn -- past
# iPXE's 15, and per NIC rather than per connection, so small chunks alone do
# not prevent it; only a poll before each Transmit does. A frame the NIC
# refuses costs an RTO (~1 s measured); a clean round measures ~10 ms.
while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
        *)        echo "  $line" ;;
    esac
done < <(python3 - "$HOST_PORT" "$WS_ROUND_BOUND_SECS" << 'PYEOF'
import base64, os, socket, sys, time

port = int(sys.argv[1])
bound = float(sys.argv[2])
K, ROUNDS, SIZE = 7, 30, 7000

def ws_open():
    s = socket.create_connection(("127.0.0.1", port), timeout=15)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws-echo HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: " + key +
               "\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        c = s.recv(4096)
        if not c:
            raise EOFError("handshake")
        resp += c
    if not resp.startswith(b"HTTP/1.1 101 "):
        raise RuntimeError(resp.split(b"\r\n")[0].decode("latin-1"))
    return s

def send_text(s, payload):
    mask = os.urandom(4)
    hdr = bytes([0x81, 0x80 | 126]) + len(payload).to_bytes(2, "big") + mask
    s.sendall(hdr + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

def recv_frame(s, buf):
    def need(n):
        while len(buf) < n:
            c = s.recv(65536)
            if not c:
                raise EOFError("frame")
            buf.extend(c)
    need(4)
    if buf[0] != 0x81 or buf[1] != 126:
        raise RuntimeError(f"unexpected frame header {bytes(buf[:2]).hex()}")
    plen = int.from_bytes(buf[2:4], "big")
    need(4 + plen)
    payload = bytes(buf[4:4 + plen])
    del buf[:4 + plen]
    return payload

try:
    socks = [ws_open() for _ in range(K)]
    bufs = [bytearray() for _ in range(K)]
    lat = []
    bad = 0
    for r in range(ROUNDS):
        payload = bytes((r * 31 + i) % 26 + 65 for i in range(SIZE))
        t0 = time.perf_counter()
        send_text(socks[0], payload)
        for s, b in zip(socks, bufs):
            if recv_frame(s, b) != payload:
                bad += 1
        lat.append(time.perf_counter() - t0)
    for s in socks:
        s.close()
    print(f"ws broadcast: {K} clients x {ROUNDS} rounds x {SIZE} B, "
          f"max round {max(lat):.3f}s, total {sum(lat):.3f}s")
    if bad == 0:
        print(f"PASS: ws broadcast: all {K * ROUNDS} echoes byte-exact")
    else:
        print(f"FAIL: ws broadcast: {bad} of {K * ROUNDS} echoes differ")
    if max(lat) < bound:
        print(f"PASS: ws broadcast: every round under {bound}s (max {max(lat):.3f}s)")
    else:
        slow = sum(1 for x in lat if x >= bound)
        print(f"FAIL: ws broadcast: {slow} round(s) at or over {bound}s "
              f"(max {max(lat):.3f}s) -- a burst across connections lost a frame")
except Exception as e:
    print(f"FAIL: ws broadcast ({e!r})")
PYEOF
)

# --- 3+4. a transfer stalled on a reader that stops reading ---
# stall_client MODE -- GET /big-stream (8 MiB: /big eight times) with a 4 KB
# receive buffer, then read NOTHING for STALL_SECS: once slirp and the host
# socket are full the guest sees a zero window, and its send stays pending
# with no progress. MODE=complete then reads the whole response and checks it;
# MODE=abort resets the connection mid-transfer. The body has to be this big:
# Linux autotunes the forwarding socket's send buffer past 1 MiB, so a stalled
# client of /big leaves the guest with nothing pending at all (measured: a
# ~1 MB Send-Q on the host, the guest already in FIN-WAIT-1).
STALL_SECS=9
stall_client() {
    python3 - "$HOST_PORT" "$STALL_SECS" "$1" << 'PYEOF'
import hashlib, socket, struct, sys, time

port, stall, mode = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
SIZE = 8 << 20
want_md5 = hashlib.md5(bytes(((i * 2654435761) & 0xffffffff) >> 24 for i in range(1 << 20)) * 8).hexdigest()
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.settimeout(30)
    s.connect(("127.0.0.1", port))
    s.sendall(b"GET /big-stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    print("stalled", flush=True)
    time.sleep(stall)
    if mode == "abort":
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
        print("aborted", flush=True)
        sys.exit(0)
    data = bytearray()
    while True:
        c = s.recv(1 << 20)
        if not c:
            break
        data.extend(c)
    s.close()
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    status = head.split(b"\r\n")[0].decode("latin-1")
    md5 = hashlib.md5(body).hexdigest()
    if status == "HTTP/1.1 200 OK" and len(body) == SIZE and md5 == want_md5:
        print(f"PASS: stalled transfer completes after a {stall:.0f}s stall: 200, {SIZE} bytes, md5 {md5}")
    else:
        print(f"FAIL: stalled transfer: want 'HTTP/1.1 200 OK'/{SIZE}/{want_md5}, got {status!r}/{len(body)}/{md5}")
except Exception as e:
    print(f"FAIL: stalled transfer ({e!r})", flush=True)
PYEOF
}
# collect_client FILE -- turn a stall client's PASS/FAIL lines into results
collect_client() {
    local line
    while IFS= read -r line; do
        case "$line" in
            "PASS:"*) pass "${line#PASS: }" ;;
            "FAIL:"*) fail "${line#FAIL: }" ;;
        esac
    done < "$1"
}
# wait_stalled FILE -- until the client has sent its request (10 s cap)
wait_stalled() {
    local i
    for i in $(seq 1 100); do
        grep -q '^stalled' "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

STALL_OUT="$TEST_TMPDIR/stall-complete.out"
stall_client complete > "$STALL_OUT" 2>&1 &
SLOW_PID=$!
wait_stalled "$STALL_OUT" || fail "stall client never sent its request"
sleep 2      # let the window close

out=$(curl -s -H "Connection: close" --max-time 10 -w "\n%{http_code} %{time_total}" \
           "${BASE_URL}/plain" 2>/dev/null || true)
PLAIN_BODY=$(head -1 <<< "$out")
read -r code secs <<< "$(tail -1 <<< "$out")"
echo "  GET /plain during the stalled transfer: code=${code:-none} secs=${secs:-?}"
if [[ "$code" == "200" && "$PLAIN_BODY" == "Hello from AxlNet" \
      && -n "$secs" && $(under "$secs" "$RESPONSIVE_BOUND_SECS") == 1 ]]; then
    pass "loop responsive during a stalled send: /plain 200 'Hello from AxlNet' in ${secs}s"
else
    fail "loop responsive during a stalled send: want 200 'Hello from AxlNet' under ${RESPONSIVE_BOUND_SECS}s, got ${code:-none} '${PLAIN_BODY}' in ${secs:-?}s"
fi

STALLED=$(sample_cores)
echo "  CPU while a send is stalled at a zero window: ${STALLED} cores"
if [[ "$STALLED" != "n/a" && $(under "$STALLED" "$CPU_BUDGET_CORES") == 1 ]]; then
    pass "a stalled send does not hold a core: ${STALLED} < ${CPU_BUDGET_CORES}"
else
    fail "a stalled send holds the CPU: ${STALLED} cores (budget ${CPU_BUDGET_CORES})"
fi
wait "$SLOW_PID" 2>/dev/null || true
SLOW_PID=0
collect_client "$STALL_OUT"

# Abort mid-transfer: the server must retire the pending send on the reset
# and keep serving.
ABORT_OUT="$TEST_TMPDIR/stall-abort.out"
STALL_SECS=3
stall_client abort > "$ABORT_OUT" 2>&1 &
SLOW_PID=$!
wait "$SLOW_PID" 2>/dev/null || true
SLOW_PID=0
grep -q '^aborted' "$ABORT_OUT" || fail "abort client did not reset its connection ($(tr '\n' ' ' < "$ABORT_OUT"))"
collect_client "$ABORT_OUT"
sleep 1
get_big "GET /big after a client abort"

# --- 5. idle after traffic ---
sleep 2
IDLE_AFTER=$(sample_cores)
echo "  idle CPU after traffic: ${IDLE_AFTER} cores (before: ${IDLE_BEFORE})"
if [[ "$IDLE_AFTER" != "n/a" && $(under "$IDLE_AFTER" "$CPU_BUDGET_CORES") == 1 ]]; then
    pass "pacing stops with nothing pending: ${IDLE_AFTER} < ${CPU_BUDGET_CORES} cores"
else
    fail "guest still busy with nothing pending: ${IDLE_AFTER} cores (budget ${CPU_BUDGET_CORES})"
fi

echo ""
printf "HTTP throughput: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    test_clean_log; echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
