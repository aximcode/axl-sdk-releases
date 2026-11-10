#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 guest=standard
# test-http-expect-qemu.sh — Expect: 100-continue (RFC 9110 10.1.1).
#
# THE DEFECT. A client that sends `Expect: 100-continue` sends the headers
# and waits for the server to say "100 Continue" (or to refuse) before it
# sends the body. The AXL server never sent one, so every such client sat
# out its own fallback timer before sending: curl -T waits 1 s per upload,
# which every PUT in the WebDAV tests paid. And a request the server was
# going to refuse anyway (no route, no credentials, too large) still had
# its body sent first.
#
# AxlTestNet.efi serve. Each case talks to the guest over a raw socket so
# the bytes on the wire are what is asserted: the headers go first, then the
# guest's first 25 bytes are read -- the length of the interim response,
# exactly `HTTP/1.1 100 Continue\r\n\r\n` -- and only if they are that does
# the body go; anything else is a final response, read to the guest's close.
#
# Usage: ./test/integration/test-http-expect-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080
# How long a case waits for the guest's answer to the headers alone: long,
# because every case but one ends that wait with bytes, and a loaded host
# must not turn a late 100 into a FAIL. The case that rightly gets NOTHING
# (HTTP/1.0) waits SILENCE_WAIT_S instead.
EXPECT_WAIT_S=5
SILENCE_WAIT_S=1

test_add_efi "$(test_build_dir)/AxlTestNet.efi"
test_use_nsh
test_build_image

echo "=== HTTP Expect: 100-continue Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
trap 'test_cleanup; true' EXIT

if ! test_wait_boot "READY" 90; then
    echo "FAIL: HTTP server did not start within ${TEST_WAITED_S} seconds"
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 2

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

BASE_URL="http://127.0.0.1:${HOST_PORT}"
CONTINUE="$TEST_TMPDIR/continue.want"
printf 'HTTP/1.1 100 Continue\r\n\r\n' > "$CONTINUE"

# exchange NAME HEAD [BODY [WAIT]] -- open a connection, send HEAD (CRLFs
# written as \r\n) and read the guest's first 25 bytes, waiting up to WAIT
# (EXPECT_WAIT_S) for them, into $TEST_TMPDIR/NAME.first. dd reads one byte
# at a time, so nothing past the 25th is consumed. If they are EXACTLY the
# 100 -- or nothing came and there is a BODY (the guest is waiting for it,
# as for HTTP/1.0) -- send BODY and read the rest, to the guest's close,
# into NAME.final. Otherwise the answer was a final response: read the rest
# of it onto NAME.first, and send no body.
exchange() {
    local name="$1" head="$2" body="${3:-}" wait="${4:-$EXPECT_WAIT_S}"
    local first="$TEST_TMPDIR/$1.first" final="$TEST_TMPDIR/$1.final"
    : > "$final"
    exec 3<>"/dev/tcp/127.0.0.1/${HOST_PORT}"
    printf '%b' "$head" >&3
    timeout "$wait" dd bs=1 count=25 status=none <&3 > "$first" || true
    if cmp -s "$first" "$CONTINUE" || [[ ! -s "$first" && -n "$body" ]]; then
        printf '%s' "$body" >&3
        timeout 10 cat <&3 > "$final" || true
    else
        timeout 10 cat <&3 >> "$first" || true
    fi
    exec 3<&-
}

# status_line FILE -- the file's first line, CR stripped.
status_line() {
    head -1 "$1" | tr -d '\r'
}

# is_continue NAME -- the guest's first answer was exactly the 100.
is_continue() {
    cmp -s "$TEST_TMPDIR/$1.first" "$CONTINUE"
}

echo ""
echo "  --- a request the server will read: 100 first, then the final response ---"
exchange post "POST /echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 5\r\nConnection: close\r\n\r\n" "hello"
is_continue post \
    && pass "POST /echo: the guest answers the headers with exactly 'HTTP/1.1 100 Continue\\r\\n\\r\\n'" \
    || fail "POST /echo: first answer '$(od -An -c "$TEST_TMPDIR/post.first" | tr -s ' \n' ' ')'"
[[ "$(status_line "$TEST_TMPDIR/post.final")" == "HTTP/1.1 200 OK" ]] \
    && [[ "$(tail -c 5 "$TEST_TMPDIR/post.final")" == "hello" ]] \
    && pass "POST /echo: then 200 with the body echoed" \
    || fail "POST /echo: final '$(status_line "$TEST_TMPDIR/post.final")', body tail '$(tail -c 5 "$TEST_TMPDIR/post.final")'"

exchange chunked "POST /echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" $'3\r\nabc\r\n0\r\n\r\n'
is_continue chunked && [[ "$(status_line "$TEST_TMPDIR/chunked.final")" == "HTTP/1.1 200 OK" ]] \
    && [[ "$(tail -c 3 "$TEST_TMPDIR/chunked.final")" == "abc" ]] \
    && pass "chunked POST /echo: 100, then 200 with the body echoed" \
    || fail "chunked POST /echo: first '$(status_line "$TEST_TMPDIR/chunked.first")', final '$(status_line "$TEST_TMPDIR/chunked.final")'"

exchange upload "POST /upload HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 4\r\nConnection: close\r\n\r\n" "data"
is_continue upload && [[ "$(status_line "$TEST_TMPDIR/upload.final")" == "HTTP/1.1 200 OK" ]] \
    && pass "streaming upload: 100, then 200" \
    || fail "streaming upload: first '$(status_line "$TEST_TMPDIR/upload.first")', final '$(status_line "$TEST_TMPDIR/upload.final")'"

exchange authok "POST /secret-echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nAuthorization: Bearer test-token\r\nContent-Length: 2\r\nConnection: close\r\n\r\n" "ok"
is_continue authok && [[ "$(status_line "$TEST_TMPDIR/authok.final")" == "HTTP/1.1 200 OK" ]] \
    && pass "authenticated POST to an auth route: 100, then 200" \
    || fail "authenticated POST: first '$(status_line "$TEST_TMPDIR/authok.first")', final '$(status_line "$TEST_TMPDIR/authok.final")'"

echo ""
echo "  --- a request the server refuses: the final status, no 100, no body read ---"
# refused NAME WANT HEAD -- the first answer is the final WANT status line,
# never the 100, and the guest closed without being sent a body.
refused() {
    local name="$1" want="$2" head="$3"
    exchange "$name" "$head" "BODY-NOT-WANTED"
    if ! is_continue "$name" && [[ "$(status_line "$TEST_TMPDIR/$name.first")" == "$want" ]] \
       && [[ ! -s "$TEST_TMPDIR/$name.final" ]] \
       && ! grep -q '100 Continue' "$TEST_TMPDIR/$name.first"; then
        pass "$name: '$want' in answer to the headers, no 100"
    else
        fail "$name: first '$(status_line "$TEST_TMPDIR/$name.first")' (want '$want'), final '$(status_line "$TEST_TMPDIR/$name.final")'"
    fi
}
refused "no route"            "HTTP/1.1 404 Not Found" \
    "POST /no-such-route HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 15\r\n\r\n"
refused "no credentials"      "HTTP/1.1 401 Unauthorized" \
    "POST /secret-echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 15\r\n\r\n"
refused "upload, no credentials" "HTTP/1.1 401 Unauthorized" \
    "POST /upload-auth HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 15\r\n\r\n"
refused "body over the limit" "HTTP/1.1 413 Payload Too Large" \
    "POST /echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 5000000\r\n\r\n"
refused "unknown expectation" "HTTP/1.1 417 Expectation Failed" \
    "POST /echo HTTP/1.1\r\nHost: guest\r\nExpect: 200-ok\r\nContent-Length: 15\r\n\r\n"
# The 401 still carries the middleware's headers, as a refusal after the body does.
grep -qxi $'X-Frame-Options: DENY\r' "$TEST_TMPDIR/no credentials.first" \
    && pass "the early 401 carries the middleware's headers" \
    || fail "the early 401 lacks the middleware's headers: $(tr -d '\r' < "$TEST_TMPDIR/no credentials.first" | tr '\n' '|')"

echo ""
echo "  --- when no 100 is owed ---"
# HTTP/1.0: the expectation MUST be ignored -- nothing until the body comes.
exchange http10 "POST /echo HTTP/1.0\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n" "10" "$SILENCE_WAIT_S"
[[ ! -s "$TEST_TMPDIR/http10.first" ]] \
    && [[ "$(status_line "$TEST_TMPDIR/http10.final")" == "HTTP/1.1 200 OK" ]] \
    && pass "HTTP/1.0: no 100; the body is read and answered 200" \
    || fail "HTTP/1.0: first '$(status_line "$TEST_TMPDIR/http10.first")', final '$(status_line "$TEST_TMPDIR/http10.final")'"
# A body sent with the headers: the client is not waiting, so no 100.
exchange eager "POST /echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc"
[[ "$(status_line "$TEST_TMPDIR/eager.first")" == "HTTP/1.1 200 OK" ]] \
    && ! grep -q '100 Continue' "$TEST_TMPDIR/eager.first" \
    && pass "a body sent with the headers: 200, no 100" \
    || fail "a body sent with the headers: first '$(status_line "$TEST_TMPDIR/eager.first")'"

echo ""
echo "  --- keep-alive: the next request on the connection after a 100 ---"
exec 3<>"/dev/tcp/127.0.0.1/${HOST_PORT}"
printf 'POST /echo HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n' >&3
timeout "$EXPECT_WAIT_S" dd bs=1 count=25 status=none <&3 > "$TEST_TMPDIR/ka.first" || true
printf 'k1GET /plain HTTP/1.1\r\nHost: guest\r\nConnection: close\r\n\r\n' >&3
timeout 10 cat <&3 > "$TEST_TMPDIR/ka.final" || true
exec 3<&-
# Counted anywhere, not per line: the echoed body "k1" has no newline, so the
# second status line follows it on the same line.
is_continue ka && [[ "$(grep -o 'HTTP/1.1 200 OK' "$TEST_TMPDIR/ka.final" | wc -l)" -eq 2 ]] \
    && [[ "$(tail -c 17 "$TEST_TMPDIR/ka.final")" == "Hello from AxlNet" ]] \
    && pass "after a 100 and its 200, the connection serves the next request" \
    || fail "keep-alive after 100: first '$(status_line "$TEST_TMPDIR/ka.first")', after: $(tr -d '\r' < "$TEST_TMPDIR/ka.final" | tr '\n' '|')"

echo ""
echo "  --- what it is for: curl -T no longer waits out its timer ---"
# curl asks for a 100 on an upload of unknown size (-T -, sent chunked) and
# on one of 1 MiB or more, then waits 1 s (--expect100-timeout) for it before
# sending the body anyway -- what every `curl -T - ` PUT in the WebDAV tests
# paid.
# Judged on what curl says it sent and saw (-v), not on the clock alone: a
# curl that stopped asking would also be quick. The time bound only shows
# the wait is gone -- 0.9 s, under its 1 s timer.
head -c 4096 /dev/zero > "$TEST_TMPDIR/up.bin"
t=$(curl -sv -o /dev/null -w '%{http_code} %{time_total}' -T - \
        -X POST "${BASE_URL}/upload" < "$TEST_TMPDIR/up.bin" 2> "$TEST_TMPDIR/curl.v" || true)
code=${t%% *}
secs=${t#* }
asked=$(grep -ci '^> Expect: 100-continue' "$TEST_TMPDIR/curl.v" || true)
saw=$(grep -c '^< HTTP/1.1 100 Continue' "$TEST_TMPDIR/curl.v" || true)
if [[ "$code" == "200" && "$asked" -eq 1 && "$saw" -eq 1 ]] \
   && awk -v s="$secs" 'BEGIN { exit !(s < 0.9) }'; then
    pass "curl -T -: asked, got the 100, 200 in ${secs} s (< 0.9, not its 1 s timer)"
else
    fail "curl -T -: '$code' in '${secs}' s, Expect sent $asked, 100 seen $saw (want 200, 1, 1, < 0.9 s)"
fi

echo ""
echo "http-expect ($TEST_ARCH): $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
