#!/bin/bash
# test-meta: arch=x64 needs= est=26 local-only=0 guest=standard
# test-webdav-fs-tls-qemu.sh — axl_http_server_serve_fs over TLS, keep-alive.
#
# The plain-HTTP test-webdav-fs-qemu.sh exercises curl's `-T -` upload (chunked
# + Expect: 100-continue) but only over http://, where a single-record body
# masks the TLS multi-record buffering bug. This boots the SAME serve_fs mount
# over HTTPS and drives the deadlock case directly: a keep-alive chunked PUT
# with Expect: 100-continue (what `curl -T -` sends by default) must complete
# (curl rc 0), not hang (rc 28). Regression guard for the TLS upload-path drain.
#
# Auxiliary single-binary test (opt out of ratchet).
#
# Usage: ./test/integration/test-webdav-fs-tls-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8443

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

# TLS build
"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all tests 2>&1 | tail -3

test_add_efi "$(test_build_dir "$_native_arch")/AxlTestNet.efi"

# The startup commands live in test-webdav-fs-tls-qemu.nsh
test_use_nsh

test_build_image

echo "=== AxlNet serve_fs WebDAV over TLS Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID  (host $HOST_PORT -> $GUEST_PORT)"
if ! test_wait_boot "READY" 60; then
    echo "FAIL: server did not start within ${TEST_WAITED_S} seconds"
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server ready"
sleep 2

BASE="https://127.0.0.1:${HOST_PORT}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# ka_code <curl args...> ; echo "<rc> <http_code>". Captures curl's exit code
# (28 on a keep-alive hang) WITHOUT letting set -e abort the script.
ka_code() {
    local out rc
    out=$(test_curl -sk --max-time 8 -o /dev/null -w '%{http_code}' "$@") \
        && rc=0 || rc=$?
    echo "$rc ${out:-000}"
}

# 1) Content-Length PUT (curl -T file) over keep-alive HTTPS — the path that
#    already works; baseline that the mount + TLS are healthy.
tmpf=$(mktemp); printf 'content-length-body' > "$tmpf"
read -r rc c < <(ka_code -T "$tmpf" "${BASE}/dav/cl.txt")
rm -f "$tmpf"
[[ "$rc" == 0 && "$c" == 2* ]] && pass "TLS keep-alive PUT (Content-Length) -> $c" \
    || fail "TLS keep-alive PUT CL hung/failed (rc=$rc, code=$c)"

# 2) THE BUG: chunked + Expect: 100-continue (what `curl -T -` sends) over
#    keep-alive HTTPS. The body's data-chunk record and the 0\r\n\r\n
#    terminator record arrive in one TCP segment; without draining the TLS
#    buffer the server re-arms a TCP recv for bytes already buffered -> hang.
read -r rc c < <(ka_code -T - "${BASE}/dav/chunked.txt" <<< "chunked-expect-body")
[[ "$rc" == 0 && "$c" == 2* ]] && pass "TLS keep-alive PUT (chunked+Expect) -> $c" \
    || fail "TLS keep-alive PUT chunked+Expect HUNG (rc=$rc, code=$c — TLS drain missing?)"

# 3) Round-trip: the chunked-uploaded bytes read back exactly.
b=$(test_curl -sk --max-time 8 "${BASE}/dav/chunked.txt" || true)
[[ "$b" == "chunked-expect-body" ]] && pass "GET returns the chunked-uploaded body" \
    || fail "GET /dav/chunked.txt round-trip ('$b')"

# 4) LARGE bodies — anything over one TLS record (16384 B) spans multiple
#    records across multiple TCP reads, which must be drained to completion.
#    Cover all four combinations: { Content-Length, chunked } x { streaming
#    upload (/dav PUT), whole-body accumulation (POST /echo) }. A drop/hang
#    surfaces as curl rc=52 / rc=28. Use 32768 B (two records).
BIG=$(mktemp); head -c 32768 /dev/zero | tr '\0' 'A' > "$BIG"   # 32 KiB of 'A'

# 4a) streaming upload, Content-Length (curl -T file, Expect disabled).
read -r rc c < <(ka_code -T "$BIG" -H "Expect:" "${BASE}/dav/big-cl.bin")
[[ "$rc" == 0 && "$c" == 2* ]] && pass "TLS large PUT /dav (Content-Length, 32K) -> $c" \
    || fail "TLS large PUT /dav (CL) dropped (rc=$rc, code=$c — multi-record body not drained?)"

# 4b) streaming upload, chunked (curl -T -). Verify the STORED size too —
#     a chunk that spans recv buffers must be decoded in full, not truncated.
read -r rc c < <(ka_code -T - "${BASE}/dav/big-chunked.bin" < "$BIG")
[[ "$rc" == 0 && "$c" == 2* ]] && pass "TLS large PUT /dav (chunked, 32K) -> $c" \
    || fail "TLS large PUT /dav (chunked) dropped (rc=$rc, code=$c)"
gz=$( { test_curl -sk --max-time 12 "${BASE}/dav/big-chunked.bin" || true; } | wc -c)
[[ "$gz" == 32768 ]] && pass "chunked-uploaded file is 32768 bytes (no chunk truncation)" \
    || fail "chunked-uploaded file truncated ($gz bytes, want 32768)"

# 4c) whole-body route, Content-Length.
read -r rc c < <(ka_code -X POST --data-binary @"$BIG" -H "Expect:" "${BASE}/echo")
[[ "$rc" == 0 && "$c" == 200 ]] && pass "TLS large POST /echo (Content-Length, 32K) -> $c" \
    || fail "TLS large POST /echo (CL) dropped (rc=$rc, code=$c)"

# 4d) whole-body route, chunked + verify the bytes round-trip intact.
sz=$( { test_curl -sk --max-time 12 -X POST -H "Transfer-Encoding: chunked" -H "Expect:" \
        --data-binary @"$BIG" "${BASE}/echo" || true; } | wc -c)
[[ "$sz" == 32768 ]] && pass "TLS large POST /echo (chunked) echoes 32768 bytes" \
    || fail "TLS large POST /echo (chunked) wrong echo size ($sz, want 32768)"
rm -f "$BIG"

# 5) MOVE over HTTPS, keep-alive. The Destination names the https URL, and
#    its authority must match the request's Host or the server answers 502.
#    Overwrite T onto an existing collection is 204 with the source there and
#    the old tree gone; Overwrite F onto an existing file is 412, unchanged.
read -r rc c < <(ka_code -T - "${BASE}/dav/mv-src.txt" <<< "moved-over-tls")
read -r rc c < <(ka_code -X MKCOL "${BASE}/dav/mv-dst")
read -r rc c < <(ka_code -T - "${BASE}/dav/mv-dst/old.txt" <<< "old")
read -r rc c < <(ka_code -X MOVE -H "Destination: ${BASE}/dav/mv-dst" -H "Overwrite: T" \
    "${BASE}/dav/mv-src.txt")
b=$(test_curl -sk --max-time 8 "${BASE}/dav/mv-dst" || true)
read -r _ co < <(ka_code "${BASE}/dav/mv-dst/old.txt")
[[ "$rc" == 0 && "$c" == 204 && "$b" == "moved-over-tls" && "$co" == 404 ]] \
    && pass "TLS MOVE onto a collection, Overwrite T -> 204, file there, old tree gone" \
    || fail "TLS MOVE Overwrite T (rc=$rc, code=$c, body '$b', old $co; want 0 204 'moved-over-tls' 404)"

read -r rc c < <(ka_code -T - "${BASE}/dav/mv-keep.txt" <<< "keep")
read -r rc c < <(ka_code -X MOVE -H "Destination: ${BASE}/dav/mv-dst" -H "Overwrite: F" \
    "${BASE}/dav/mv-keep.txt")
b=$(test_curl -sk --max-time 8 "${BASE}/dav/mv-dst" || true)
bk=$(test_curl -sk --max-time 8 "${BASE}/dav/mv-keep.txt" || true)
[[ "$rc" == 0 && "$c" == 412 && "$b" == "moved-over-tls" && "$bk" == "keep" ]] \
    && pass "TLS MOVE onto an existing file, Overwrite F -> 412, both unchanged" \
    || fail "TLS MOVE Overwrite F (rc=$rc, code=$c, dest '$b', source '$bk'; want 0 412 'moved-over-tls' 'keep')"

# Two requests PIPELINED in one TLS write -- one record, the second request
# decrypted with the first -- are both answered, in order: the plaintext
# past the first request is the next one, never dropped.
read -r _ _ < <(ka_code -T - "${BASE}/dav/tp1.txt" <<< "tls first")
read -r _ _ < <(ka_code -T - "${BASE}/dav/tp2.txt" <<< "tls second")
# The control: one request the same way, so a failure below is the
# pipelining and not the client.
P0=$(printf 'GET /dav/tp1.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' \
    | timeout 15 openssl s_client -quiet -ign_eof -connect "127.0.0.1:${HOST_PORT}" 2>/dev/null \
    | tr -d '\r' | grep -E '^(HTTP/1\.1 |tls )' | tr '\n' '|' || true)
[[ "$P0" == "HTTP/1.1 200 OK|tls first|" ]] \
    && pass "TLS: one GET through openssl s_client -> 200 'tls first' (the pipelining test's control)" \
    || fail "TLS s_client control (got '$P0')"
P=$(printf 'GET /dav/tp1.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\nGET /dav/tp2.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' \
    | timeout 15 openssl s_client -quiet -ign_eof -connect "127.0.0.1:${HOST_PORT}" 2>"$TEST_TMPDIR/sc.err" \
    | tee "$TEST_TMPDIR/sc.out" | tr -d '\r' | grep -E '^(HTTP/1\.1 |tls )' | tr '\n' '|' || true)
[[ "$P" == "HTTP/1.1 200 OK|tls first|HTTP/1.1 200 OK|tls second|" ]] \
    && pass "TLS: two pipelined GETs in one write -> both answered, in order" \
    || fail "TLS pipelining (got '$P'; want 200 'tls first' 200 'tls second'; s_client: $(head -c 300 "$TEST_TMPDIR/sc.err" | tr '\n' ' ') out: $(head -c 300 "$TEST_TMPDIR/sc.out" | tr '\r\n' '  '))"

# Carried bytes keep their ORDER: request A's headers less their final CRLF
# in one write, then the CRLF with B and C behind it -- more than the header
# room A left -- in the next. The decrypted batch overflows A's header room
# (its tail carried), and the bytes past A in the header buffer are carried
# too: they come FIRST in the stream, so they must go in front. Expect A, B
# and C answered, in order, byte-exact.
read -r _ _ < <(ka_code -T - "${BASE}/dav/tp3.txt" <<< "tls third")
# A carries a 3500-byte header, so the room it leaves (~550 bytes) is far
# smaller than any record the next write can arrive in: the overflow is
# certain, not a matter of how the client cuts its records.
PADA=$(head -c 3500 /dev/zero | tr '\0' 'a')
PADB=$(head -c 1000 /dev/zero | tr '\0' 'b'); PADC=$(head -c 700 /dev/zero | tr '\0' 'c')
P3=$( { printf 'GET /dav/tp1.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: %s\r\n' "$PADA"; sleep 1;
        printf '\r\nGET /dav/tp2.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: %s\r\n\r\nGET /dav/tp3.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: %s\r\nConnection: close\r\n\r\n' "$PADB" "$PADC"; } \
    | timeout 15 openssl s_client -quiet -ign_eof -connect "127.0.0.1:${HOST_PORT}" 2>/dev/null \
    | tr -d '\r' | grep -E '^(HTTP/1\.1 |tls )' | tr '\n' '|' || true)
[[ "$P3" == "HTTP/1.1 200 OK|tls first|HTTP/1.1 200 OK|tls second|HTTP/1.1 200 OK|tls third|" ]] \
    && pass "TLS: A split before its last CRLF, then B and C past the header room -> A, B, C in order" \
    || fail "TLS carry order (got '$P3'; want 200 first, 200 second, 200 third)"

# A record larger than the header buffer: a 6000-byte POST /echo and a GET
# behind it, one write. The plaintext past the 4 KiB the header phase can
# take is carried (it was dropped), then read as the body and the next
# request: 200 with all 6000 bytes, then 200 'tls second'.
X6=$(head -c 6000 /dev/zero | tr '\0' 'x')
P2=$(printf 'POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 6000\r\n\r\n%sGET /dav/tp2.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' "$X6" \
    | timeout 15 openssl s_client -quiet -ign_eof -connect "127.0.0.1:${HOST_PORT}" 2>/dev/null \
    | tr -d '\r' | sed -E 's/^x{6000}/X6000/' | grep -E '^(HTTP/1\.1 |tls |X6000)' | tr '\n' '|' || true)
# (The echoed body has no newline, so the next status line follows it on
# the same line.)
[[ "$P2" == "HTTP/1.1 200 OK|X6000HTTP/1.1 200 OK|tls second|" ]] \
    && pass "TLS: a 6000-byte POST and a GET in one write -> the whole body echoed, then the GET answered" \
    || fail "TLS record past the header buffer (got '${P2:0:200}'; want 200 X6000 200 'tls second')"

echo ""
echo "  serve_fs WebDAV over TLS: $PASS passed, $FAIL failed ($TEST_ARCH)"
if [[ "$FAIL" -ne 0 ]]; then
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
fi
[[ "$FAIL" -eq 0 && "$PASS" -gt 0 ]] && exit 0 || exit 1
