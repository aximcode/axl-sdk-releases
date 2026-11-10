#!/bin/bash
# test-meta: arch=both needs= est=95 local-only=0 guest=standard
# test-http-error-leak-qemu.sh — a refused request frees what middleware set.
#
# THE DEFECT (security-relevant: remote and UNAUTHENTICATED). The HTTP
# dispatcher runs the middleware pipeline on the response before it routes
# and authenticates, and a middleware may set headers or a body -- a
# consumer's security-headers middleware sets four headers on every
# response. Only the dispatch tail freed that state. The 404, the 401/403
# and the response-cache hit all returned early, so every refused request
# leaked the middleware's header table: 14 allocations / 920 B per request
# in SoftBMC (four headers). A port scanner, or a browser tab holding a stale
# token, exhausts the heap with no credentials at all. A cache hit also
# dropped a body the middleware had set, by overwriting the pointer.
#
# AxlTestNet.efi serve runs test_headers_middleware (four headers, plus a
# body on request) on every response and serves GET /heap, the live AxlMem
# allocation count (a header, no body, so never cached). After a warm-up of
# all the traffic, each class is read, sent N times and read again: the
# count must be EXACTLY unchanged. The control is /heap
# itself. Also checked, because the same function had them: the cache does
# not answer an authenticated route (it served /secret to anyone once an
# authorised read had filled it), only GET is cached, and the 404/401
# carry the middleware's headers, which error responses used to drop.
#
# Usage: ./test/integration/test-http-error-leak-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080
# 50 requests a class, and the count must be EXACTLY unchanged: a leak of
# one allocation a request shows as 50 (the defect leaked 14 a request).
N=50

test_add_efi "$(test_build_dir)/AxlTestNet.efi"

# Startup: init network, run DHCP, start the HTTP server
# (test-http-error-leak-qemu.nsh)
test_use_nsh

test_build_image

echo "=== HTTP Error-Path Leak Test ($TEST_ARCH) ==="

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
CURL_OPTS=(-s -H "Connection: close" --max-time 10)

# probe -- one GET /heap: "COUNT PENDING", the live allocation count
# (X-Heap-Count) and the guest's graceful closes not yet finalized
# (X-Close-Pending, axl_tcp_closes_pending()) at the same instant, or empty
# if the read failed. WITHOUT Connection: close, so curl closes first and the
# guest's close of the probe is the passive one, done in a round trip.
probe() {
    { curl -s --max-time 10 -o /dev/null -D - "${BASE_URL}/heap" 2>/dev/null || true; } \
        | tr -d '\r' | awk -F': ' '
            tolower($1) == "x-heap-count"    { h = $2 }
            tolower($1) == "x-close-pending" { p = $2 }
            END { if (h ~ /^[0-9]+$/ && p ~ /^[0-9]+$/) print h, p }'
}

# A connection the guest closes first holds its state (3 allocations) until
# the guest's loop finalizes the close, and every request below sends
# Connection: close, so each leaves one -- for a round trip once the FINs are
# exchanged (it used to be TIME_WAIT, 2 s, and this script slept 4 s before
# every reading; two readings that agreed were still 3-15 allocations high).
# A reading cannot tell a pending close from a leak, so it is taken only when
# the guest says none is pending: then the count holds exactly what is live.
#
# heap -- the allocation count from the first probe that reports no pending
# close, or empty if none does within HEAP_SETTLE_S (a close the guest never
# finalizes is a defect too, and the class then fails on the empty reading).
HEAP_POLL_S=0.05
HEAP_SETTLE_S=10
heap() {
    local h p end=$((SECONDS + HEAP_SETTLE_S))
    while :; do
        read -r h p <<< "$(probe)"
        if [[ -n "$p" && "$p" -eq 0 ]]; then
            echo "$h"
            return
        fi
        (( SECONDS < end )) || return
        sleep "$HEAP_POLL_S"
    done
}

# settle_ms -- milliseconds until the guest reports no pending close, or
# empty past HEAP_SETTLE_S.
settle_ms() {
    local h p t0=$EPOCHREALTIME end=$((SECONDS + HEAP_SETTLE_S))
    while :; do
        read -r h p <<< "$(probe)"
        if [[ -n "$p" && "$p" -eq 0 ]]; then
            echo $(( (10#${EPOCHREALTIME//[.,]/} - 10#${t0//[.,]/}) / 1000 ))
            return
        fi
        (( SECONDS < end )) || return
        sleep "$HEAP_POLL_S"
    done
}

# code ARGS... -- one request's status code.
code() {
    curl "${CURL_OPTS[@]}" -o /dev/null -w '%{http_code}' "$@" 2>/dev/null || true
}

# dead WHERE -- the server stopped answering (a fault or hang in the guest):
# report it and stop, rather than let every later request wait out its
# --max-time -- a wedged guest once ran this script for half an hour.
dead() {
    fail "the server stopped answering ($1)"
    echo "http-error-leak ($TEST_ARCH): $PASS passed, $FAIL failed"
    exit 1
}

# leak_class NAME WANT_CODE CURL_ARGS... -- warm once, then N requests that
# must each answer WANT_CODE and must leave the allocation count unchanged.
leak_class() {
    local name="$1" want="$2" before after bad=0 got
    shift 2
    got=$(code "$@")
    [[ "$got" == "000" ]] && dead "$name: no answer to the warm-up request"
    if [[ "$got" != "$want" ]]; then
        fail "$name: warm-up answered $got, not $want"
        return
    fi
    before=$(heap)
    for _ in $(seq 1 "$N"); do
        got=$(code "$@")
        [[ "$got" == "000" ]] && dead "$name: no answer"
        [[ "$got" == "$want" ]] || bad=$((bad + 1))
    done
    after=$(heap)
    if [[ -z "$before" || -z "$after" ]]; then
        fail "$name: could not read /heap (before='$before' after='$after')"
    elif [[ "$bad" -ne 0 ]]; then
        fail "$name: $bad of $N requests did not answer $want"
    elif [[ "$after" -eq "$before" ]]; then
        pass "$name: $N requests ($want), live allocations $before -> $after"
    else
        fail "$name: $N requests ($want) changed the heap by $((after - before)) allocations ($before -> $after)"
    fi
}

# WARM-UP. First-use allocations (pools, lazily built tables) are not a
# leak: run each class 120 times first, so the measured runs start from
# steady state and a leak of even one allocation a request shows as N.
for _ in 1 2 3; do
    for _ in $(seq 1 40); do
        [[ "$(code "${BASE_URL}/heap")" == 200 ]] || dead "warm-up"
        code "${BASE_URL}/no-such-route" > /dev/null
        code "${BASE_URL}/secret" > /dev/null
        code -H "Authorization: Bearer wrong" "${BASE_URL}/secret" > /dev/null
        code -H "X-Test-MW-Body: 1" "${BASE_URL}/cached" > /dev/null
    done
done
head -c 4096 /dev/zero > "$TEST_TMPDIR/up.bin"
for _ in $(seq 1 40); do
    [[ "$(code "${BASE_URL}/heap")" == 200 ]] || dead "upload warm-up"
    code -X POST --data-binary "@$TEST_TMPDIR/up.bin" "${BASE_URL}/upload-auth" > /dev/null
    code -X POST --data-binary "@$TEST_TMPDIR/up.bin" "${BASE_URL}/upload" > /dev/null
done

echo ""
echo "  --- control: the reader itself does not move the count ---"
leak_class "GET /heap (no body, never cached)" 200 "${BASE_URL}/heap"

echo ""
echo "  --- the guest finalizes the connections it closes at the FIN exchange ---"
# A raw connection the guest answers with Connection: close and closes FIRST:
# the read below ends on the guest's FIN, so the guest is the active closer
# every time. (A curl request is not a fixture for this: curl closes on
# Content-Length too and wins the race about half the time, which makes the
# guest the PASSIVE closer -- done in a round trip under either shape.)
#
# CONTROL: the probe can SEE a pending close. The host side keeps its end
# open, so slirp sends the guest no FIN and the guest's close sits in
# FIN_WAIT_2 -- pending, for the 500 ms the close gives a peer to answer.
settle_ms > /dev/null
exec 3<>"/dev/tcp/127.0.0.1/${HOST_PORT}"
printf 'GET /no-such-route HTTP/1.1\r\nHost: guest\r\nConnection: close\r\n\r\n' >&3
timeout 5 cat <&3 > /dev/null || true
read -r _ held <<< "$(probe)"
# At least 1, not exactly: the last settling probe's own (passive) close can
# still be pending for one 10 ms sweep.
[[ -n "$held" && "$held" -ge 1 ]] \
    && pass "a close the peer has not answered is reported pending ($held)" \
    || fail "a close the peer has not answered: X-Close-Pending '$held', not >= 1"
# THE BOUND: the host closes, slirp sends the guest its FIN, the guest
# reaches TIME_WAIT -- and the close must be finalized then. It used to wait
# out TIME_WAIT, 2 s on EDK2 (measured 1.9-2.1 s from here). 300 ms also
# fails a close finalized only at the FIN_WAIT_2 grace, ~450 ms from here
# (500 ms from the guest's FIN, which the control read above already took
# part of); TIME_WAIT is seen within a sweep (measured 39-50 ms).
exec 3<&-
ms=$(settle_ms)
if [[ -n "$ms" && "$ms" -lt 300 ]]; then
    pass "an active close is finalized ${ms} ms after the peer's FIN (< 300: at TIME_WAIT)"
else
    fail "an active close took '${ms}' ms after the peer's FIN to finalize (bound 300)"
fi

echo ""
echo "  --- the cache never answers an authenticated route ---"
# An authorised read of /secret first: before the fix it FILLED the cache,
# and every unauthenticated read after it got the secret, 200.
code -H "Authorization: Bearer test-token" "${BASE_URL}/secret" > /dev/null
got=$(code "${BASE_URL}/secret")
[[ "$got" == "401" ]] \
    && pass "after an authorised read, an unauthenticated GET /secret is 401" \
    || fail "an unauthenticated GET /secret answered $got after an authorised one (served from the cache)"

echo ""
echo "  --- refused requests ---"
leak_class "404, unrouted path"        404 "${BASE_URL}/no-such-route"
leak_class "401, no credentials"       401 "${BASE_URL}/secret"
leak_class "401, wrong token"          401 -H "Authorization: Bearer wrong" "${BASE_URL}/secret"
# /cached is a cached route: the first read fills the cache, the rest are
# hits. The middleware ALSO sets a body here, which a hit used to overwrite.
# A key of its own (the query is part of the cache key), filled right before
# the class: the entry the warm-up filled is older than the 60 s default TTL
# by now on aa64, and expired in the middle of the class.
HIT_URL="${BASE_URL}/cached?hit-proof=1"
c0=$(curl "${CURL_OPTS[@]}" "$HIT_URL" 2>/dev/null || true)
leak_class "cache hit, middleware set a body" 200 -H "X-Test-MW-Body: 1" "$HIT_URL"
c1=$(curl "${CURL_OPTS[@]}" "$HIT_URL" 2>/dev/null || true)
# The handler counts its runs into the body: unchanged across the class means
# every one of those requests WAS a cache hit, not the handler path.
if [[ -n "$c0" && "$c0" == "$c1" ]]; then
    pass "the cache-hit class was served from the cache throughout ($c1)"
else
    fail "the cache-hit class reached the handler: '$c0' then '$c1'"
fi

echo ""
echo "  --- streaming uploads: the same leak, on its own path ---"
# Upload routes run the middleware on their own, before the body streams,
# and dropped its state on every exit -- the 401 included.
head -c 4096 /dev/zero > "$TEST_TMPDIR/up.bin"
leak_class "401, refused upload"   401 -X POST --data-binary "@$TEST_TMPDIR/up.bin" \
    "${BASE_URL}/upload-auth"
leak_class "a completed upload"    200 -X POST --data-binary "@$TEST_TMPDIR/up.bin" \
    "${BASE_URL}/upload"
hdrs=$({ curl "${CURL_OPTS[@]}" -o /dev/null -D - -X POST --data-binary "@$TEST_TMPDIR/up.bin" \
           -H "X-Test-MW-Framing: 1" "${BASE_URL}/upload" 2>/dev/null || true; } | tr -d '\r')
if grep -qxi 'X-Frame-Options: DENY' <<< "$hdrs"; then
    pass "a completed upload's response carries the middleware's headers"
else
    fail "a completed upload's response lacks the middleware's headers: $(tr '\n' '|' <<< "$hdrs")"
fi
# ... but not its framing: the upload's response writes its own.
if ! grep -qi '^Content-Length: 99' <<< "$hdrs" \
   && [[ "$(grep -ci '^Content-Length:' <<< "$hdrs")" -eq 1 ]] \
   && [[ "$(grep -ci '^Connection:' <<< "$hdrs")" -eq 1 ]]; then
    pass "a completed upload's response keeps its own framing"
else
    fail "a completed upload's response carried the middleware's framing: $(tr '\n' '|' <<< "$hdrs")"
fi
# A handler's STATIC body is the handler's: the release after the send used
# to axl_free it.
leak_class "an upload answered with a static body" 200 -X POST \
    --data-binary "@$TEST_TMPDIR/up.bin" "${BASE_URL}/upload-static"
body=$(curl "${CURL_OPTS[@]}" -X POST --data-binary "@$TEST_TMPDIR/up.bin" \
       "${BASE_URL}/upload-static" 2>/dev/null || true)
[[ "$body" == '{"static":true}' ]] \
    && pass "the static upload body arrives intact" \
    || fail "the static upload body: '$body'"

echo ""
echo "  --- only GET is cached ---"
# The cache key holds no request body: a cached POST answered a different
# body's repeat without running the handler.
p1=$(curl "${CURL_OPTS[@]}" -X POST -d 'first-body' "${BASE_URL}/echo" 2>/dev/null || true)
p2=$(curl "${CURL_OPTS[@]}" -X POST -d 'second-body' "${BASE_URL}/echo" 2>/dev/null || true)
if grep -q 'second-body' <<< "$p2" && ! grep -q 'first-body' <<< "$p2"; then
    pass "a second POST /echo is answered by the handler, not the cache"
else
    fail "POST /echo was served from the cache (first: $p1 / second: $p2)"
fi

echo ""
echo "  --- error responses carry the middleware's headers ---"
for path in /no-such-route /secret; do
    hdrs=$({ curl "${CURL_OPTS[@]}" -o /dev/null -D - "${BASE_URL}${path}" 2>/dev/null || true; } \
           | tr -d '\r')
    if grep -qxi 'X-Content-Type-Options: nosniff' <<< "$hdrs" \
       && grep -qxi 'X-Frame-Options: DENY' <<< "$hdrs"; then
        pass "GET $path ($(head -1 <<< "$hdrs" | cut -d' ' -f2)) carries the middleware's headers"
    else
        fail "GET $path lacks the middleware's headers: $(tr '\n' '|' <<< "$hdrs")"
    fi
done

echo ""
echo "  --- framing headers from middleware are dropped from errors ---"
# `|| true` inside the pipe: curl exits 18 when a Content-Length promises
# bytes that never come -- the very defect -- and under the suite's
# pipefail that ended the script before this assertion could report it.
hdrs=$({ curl "${CURL_OPTS[@]}" --max-time 5 -o /dev/null -D - \
           -H "X-Test-MW-Framing: 1" "${BASE_URL}/no-such-route" 2>/dev/null || true; } | tr -d '\r')
if grep -qx 'Content-Length: 0' <<< "$hdrs" && ! grep -qi '^Content-Length: 99' <<< "$hdrs" \
   && [[ "$(grep -ci '^Connection:' <<< "$hdrs")" -eq 1 ]]; then
    pass "a 404 keeps its own Content-Length: 0 and one Connection header"
else
    fail "a 404 carried the middleware's framing: $(tr '\n' '|' <<< "$hdrs")"
fi

echo ""
echo "  --- a header block too large is a 500, not an overrun ---"
hdrs=$({ curl "${CURL_OPTS[@]}" -o "$TEST_TMPDIR/big.body" -D - "${BASE_URL}/bigheader" \
           2>/dev/null || true; } | tr -d '\r')
after=$(code "${BASE_URL}/heap")
if [[ "$(head -1 <<< "$hdrs" | cut -d' ' -f2)" == "500" ]] \
   && grep -qx 'Content-Length: 0' <<< "$hdrs" && grep -qxi 'Connection: close' <<< "$hdrs" \
   && [[ ! -s "$TEST_TMPDIR/big.body" ]] && [[ "$after" == "200" ]]; then
    pass "a 3 KB response header answers a bodyless 500, closes, and the server keeps serving"
else
    fail "oversized header block: $(tr '\n' '|' <<< "$hdrs") body=$(wc -c < "$TEST_TMPDIR/big.body"), then /heap $after"
fi

echo ""
echo "http-error-leak ($TEST_ARCH): $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
