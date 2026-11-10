#!/bin/bash
# test-meta: arch=x64 needs=openssl est=174 local-only=0 guest=standard
# test-http-client.sh -- the UEFI HTTP client, one part of the HTTP suite: the guest
# fetches from host-server.py (plain and TLS, through the QEMU gateway
# 10.0.2.2) and reports what it read, through the HTTP server.
#
# The boot and the helpers are lib/http-test-lib.sh, whose header says
# how the suite is split and why.
#
# Usage: ./test/integration/test-http-client.sh [--arch X64|AARCH64]

HTTP_TEST_PART="client"
HTTP_TEST_HOST_SERVERS=1
source "$(dirname "$0")/lib/http-test-lib.sh"
test_use_nsh
http_boot

# ---------------------------------------------------------------------------
# UEFI HTTP Client tests (UEFI fetches from host Python server)
# ---------------------------------------------------------------------------

echo ""
echo "  --- UEFI HTTP Client Tests ---"

# The request target is sent WHOLE, however long. It was built in a
# 512-byte buffer and silently cut, so a 512-byte path asked for its
# 511-byte prefix -- another resource. The host answers with the length of
# the target as it arrived; 511 (the old fit), 512 (one over) and 1500,
# through the async core (axl_http_get) and the sync streaming path.
CP=""
for mode in get stream; do
    for n in 511 512 1500; do
        b=$(curl "${CURL_OPTS[@]}" "${BASE_URL}/client-path?port=${HOST_SERVER_PORT}&n=${n}&mode=${mode}" 2>/dev/null || true)
        CP+="${mode}/${n}=${b} "
    done
done
want='get/511={"len": 511} get/512={"len": 512} get/1500={"len": 1500} stream/511={"len": 511} stream/512={"len": 512} stream/1500={"len": 1500} '
[[ "$CP" == "$want" ]] \
    && pass "the client sends a 511-, 512- and 1500-byte request target whole (get and streaming)" \
    || fail "request target length (got '$CP'; want '$want')"

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

# A RESPONSE is read with a user agent's rules (RFC 9112), by both clients
# -- the async core (axl_http_get) and the sync streaming path: an obs-fold
# is unfolded to one SP (5.2), identical Content-Lengths (two lines, or a
# "5, 5" list) are one length and differing ones fail the response (6.3),
# bare-LF line ends are read (2.2), and a Transfer-Encoding whose last
# coding is not chunked runs to the close, its Content-Length ignored (6.3).
cfetch() {
    curl "${CURL_OPTS[@]}" "${BASE_URL}/client-fetch?port=${HOST_SERVER_PORT}&path=/resp/$1&mode=$2${3:+&$3}" 2>/dev/null || true
}
RF=""
for mode in get stream; do
    RF+="$mode fold=$(cfetch fold "$mode" hdr=x-folded) "
    RF+="dup=$(cfetch dup-cl "$mode") list=$(cfetch list-cl "$mode") diff=$(cfetch diff-cl "$mode") "
    RF+="lf=$(cfetch bare-lf "$mode" hdr=x-lf) close=$(cfetch te-close "$mode") "
done
want='get fold=200:5:hello:a b c| dup=200:5:hello| list=200:5:hello| diff=err:-1| lf=200:5:hello:y| close=200:17:hello-until-close| '
want+='stream fold=200:5:hello:a b c| dup=200:5:hello| list=200:5:hello| diff=err:-1| lf=200:5:hello:y| close=200:17:hello-until-close| '
[[ "$RF" == "$want" ]] \
    && pass "client (get and streaming): a fold unfolded, 5+5 and '5, 5' read as 5, 5+6 refused, bare LF read, TE gzip runs to the close" \
    || fail "client response rules (got '$RF'; want '$want')"

# A body framed by the close ends well only at an ORDERLY close (a FIN):
# cut by a reset part-way, the request fails AXL_INCOMPLETE (-11) through
# both clients -- it was returned as a whole body, "partial". Control: the
# same framing closed with a FIN still succeeds.
RR=""
for mode in get stream; do
    RR+="$mode $(cfetch te-close-rst "$mode") $(cfetch te-close "$mode") ; "
done
want='get err:-11| 200:17:hello-until-close| ; stream err:-11| 200:17:hello-until-close| ; '
[[ "$RR" == "$want" ]] \
    && pass "client (get and streaming): a close-framed body cut by a reset fails AXL_INCOMPLETE; closed by a FIN it succeeds" \
    || fail "client read-to-close reset (got '$RR'; want '$want')"

# RFC 9112 6.3 rule 8: a response with neither Content-Length nor
# Transfer-Encoding has a body that runs to the close -- it was read as
# having none -- and cut by a reset it fails AXL_INCOMPLETE. Rule 1's
# exceptions keep no body, on a connection the host leaves OPEN (a client
# waiting for a body would hang to its timeout): a response to HEAD, with
# or without a Content-Length, a 204 and a 304. Through both clients.
R8=""
for mode in get stream; do
    R8+="$mode $(cfetch close-200 "$mode") $(cfetch close-200-rst "$mode") "
    R8+="$(cfetch head-ka "$mode" method=HEAD) $(cfetch head-cl-ka "$mode" method=HEAD) "
    R8+="$(cfetch 204-ka "$mode") $(cfetch 304-ka "$mode") ; "
done
want='get 200:22:body-runs-to-the-close| err:-11| 200:0:| 200:0:| 204:0:| 304:0:| ; '
want+='stream 200:22:body-runs-to-the-close| err:-11| 200:0:| 200:0:| 204:0:| 304:0:| ; '
[[ "$R8" == "$want" ]] \
    && pass "client (get and streaming): no CL/TE runs to the close, cut by a reset fails; HEAD (with CL too), 204, 304 have no body and do not hang" \
    || fail "client rule 8 (got '$R8'; want '$want')"

# Interim 1xx heads (RFC 9110 15.2) are read past to the final response:
# a 100 (its final head in a later segment), and a 103 then a 100 in one
# segment, each followed by a SECOND request on the same connection --
# both answered 200 in order, request 1 then 2 (a 1xx taken for the final
# response leaves its 200 to be read as the next request's). More than the
# cap (16) of them fail the request, and the connection is dropped (the next
# request fails the same way, not reads the 200 left behind); so does an
# unrequested 101.
I1=""
for mode in get stream; do
    I1+="$mode $(cfetch 100-200-ka "$mode" n=2) $(cfetch 103-100-200-ka "$mode" n=2) "
    I1+="$(cfetch 1xx-flood-ka "$mode" n=2) $(cfetch 101-ka "$mode") ; "
done
P='200:[0-9]+:c([0-9]+)r1\|200:[0-9]+:c([0-9]+)r2\|'
RE="^get ${P} ${P} err:-1\|err:-1\| err:-1\| ; stream ${P} ${P} err:-1\|err:-1\| err:-1\| ; $"
if [[ "$I1" =~ $RE ]] \
   && [[ "${BASH_REMATCH[1]}" == "${BASH_REMATCH[2]}" && "${BASH_REMATCH[3]}" == "${BASH_REMATCH[4]}" \
      && "${BASH_REMATCH[5]}" == "${BASH_REMATCH[6]}" && "${BASH_REMATCH[7]}" == "${BASH_REMATCH[8]}" ]]; then
    pass "client (get and streaming): 100, and 103+100, skipped to the 200, the next request on the connection gets its own; 17 1xx and a 101 fail"
else
    fail "client interim 1xx (got '$I1'; want each pair 200 r1 then r2 on one connection, then err:-1 err:-1)"
fi

# The idle deadline firing mid-body is AXL_TIMEOUT (-3) through both
# clients -- the streaming path called a server that sent no length and
# did not close a truncation (AXL_INCOMPLETE). A body framed by the close,
# and one framed by Content-Length, each stall past a 2 s timeout.
TO=""
for mode in get stream; do
    TO+="$mode $(cfetch close-200-stall "$mode" opt=timeout.ms:2000) $(cfetch cl-stall "$mode" opt=timeout.ms:2000) ; "
done
want='get err:-3| err:-3| ; stream err:-3| err:-3| ; '
[[ "$TO" == "$want" ]] \
    && pass "client (get and streaming): an idle deadline mid-body is AXL_TIMEOUT, close-framed and Content-Length alike" \
    || fail "client mid-body timeout (got '$TO'; want '$want')"

# Over TLS, a body framed by the close ends in order only at close_notify.
# Strict (the default): a bare TCP close is AXL_INCOMPLETE (-11), and the
# error names the cause; close_notify ends it whole; a Content-Length body
# is unaffected. tls.close_notify=lenient: the bare close ends it, with a
# WARNING; a reset is still AXL_INCOMPLETE; Content-Length and chunked
# bodies are unaffected. Through both clients.
ctls() {
    curl "${CURL_OPTS[@]}" "${BASE_URL}/client-fetch?port=${HOST_TLS_PORT}&scheme=https&path=$1&mode=$2&opt=tls.verify:false${3:+;$3}" 2>/dev/null || true
}
LN='tls.close_notify:lenient'
CN=""
for mode in get stream; do
    CN+="$mode strict $(ctls /resp/te-close-nonotify "$mode") $(ctls /resp/te-close-notify "$mode") $(ctls /resp/dup-cl "$mode") "
    CN+="lenient $(ctls /resp/te-close-nonotify "$mode" "$LN") $(ctls /resp/te-close-rst "$mode" "$LN") "
    CN+="$(ctls /resp/dup-cl "$mode" "$LN") $(ctls /chunked "$mode" "$LN") ; "
done
w='strict err:-11| 200:18:notify-body-intact| 200:5:hello| lenient 200:18:cut-body-no-notify| err:-11| 200:5:hello| 200:20:hello-chunk-second!!| ; '
want="get $w""stream $w"
test_clean_log
n_strict=$(grep -cF 'TLS ended without close_notify after 18 bytes of a body framed by the close (set tls.close_notify=lenient to accept it)' "$TEST_CLEAN_LOG" || true)
n_lenient=$(grep -cF 'TLS ended without close_notify after 18 bytes of a body framed by the close; accepted (tls.close_notify=lenient)' "$TEST_CLEAN_LOG" || true)
[[ "$CN" == "$want" && "$n_strict" == 2 && "$n_lenient" == 2 ]] \
    && pass "client over TLS (get and streaming): close_notify required by default, the error naming it; lenient accepts a bare close with a warning, not a reset" \
    || fail "client TLS close_notify (got '$CN'; want '$want'; strict errors $n_strict, lenient warnings $n_lenient, want 2 and 2)"

# A body framed by Content-Length is unaffected by tls.close_notify=lenient:
# cut by a FIN without close_notify over TLS, it still FAILS (lenient applies
# only to a body framed by the close). Through both clients.
CC=""
for mode in get stream; do
    CC+="$mode $(ctls /resp/cl-cut-nonotify "$mode" "$LN") "
done
[[ "$CC" == "get err:-1| stream err:-1| " ]] \
    && pass "client over TLS, lenient (get and streaming): a Content-Length body cut by a bare close still fails" \
    || fail "client TLS lenient CL cut (got '$CC'; want 'get err:-1| stream err:-1| ')"

# A request answered 100 Continue and then a reset, on a REUSED connection,
# is not replayed as if the connection had been stale: the server has it.
# The host counts arrivals: the connection's first request (200) and the
# second (100, reset) -- exactly 2, and the second request fails.
RP=""
for mode in get stream; do
    # The 200's body names its connection (cNr1); drop the number.
    r=$(cfetch 100-rst-ka "$mode" n=2 | sed -E 's/^200:[0-9]+:c[0-9]+r1\|/200:r1|/')
    a=$(curl -s "http://127.0.0.1:${HOST_SERVER_PORT}/arrivals/100-rst-ka" || true)
    RP+="$mode $r arrivals=$a ; "
done
[[ "$RP" == "get 200:r1|err:-1| arrivals=2 ; stream 200:r1|err:-1| arrivals=2 ; " ]] \
    && pass "client (get and streaming): a 100 then a reset on a reused connection reaches the server once, and fails" \
    || fail "client 100+reset replay (got '$RP'; want each mode 200 then err:-1, arrivals=2)"

# The request's Host is the target's authority (RFC 9110 7.2): host:port
# when the port is not the scheme's default -- it was the bare host, and a
# server comparing a Destination's host:port against it took its own URL
# for another host. Through both clients; and after a redirect to the TLS
# instance on another port, the Host follows the new target. (A default
# port and an IPv6 literal cannot be reached from the guest: the host
# fixtures listen on unprivileged ports, and the UEFI stack is IPv4 only;
# the unit tests pin both.)
HE=""
for mode in get stream; do
    HE+="$mode $(cfetch host-echo "$mode") "
done
HE+="redirect $(cfetch "redirect-tls?port=${HOST_TLS_PORT}" get opt=tls.verify:false)"
hp="10.0.2.2:${HOST_SERVER_PORT}"
tp="10.0.2.2:${HOST_TLS_PORT}"
want="get 200:${#hp}:${hp}| stream 200:${#hp}:${hp}| redirect 200:${#tp}:${tp}|"
[[ "$HE" == "$want" ]] \
    && pass "client Host: host:port for a non-default port (get and streaming), and the new target's after a redirect" \
    || fail "client Host (got '$HE'; want '$want')"

# Content-Length with Transfer-Encoding: chunked frames the body, and the
# connection is never reused (6.3) -- the second request goes out on a new
# connection, which the host numbers. Control: chunked alone on a kept-open
# connection IS reused, so the numbering can see reuse.
RK=""
for mode in get stream; do
    RK+="$mode $(cfetch cl-te-ka "$mode" n=2) / $(cfetch te-ka "$mode" n=2) ; "
done
if [[ "$RK" =~ ^get\ 200:[0-9]+:conn-([0-9]+)\|200:[0-9]+:conn-([0-9]+)\|\ /\ 200:[0-9]+:conn-([0-9]+)\|200:[0-9]+:conn-([0-9]+)\|\ \;\ stream\ 200:[0-9]+:conn-([0-9]+)\|200:[0-9]+:conn-([0-9]+)\|\ /\ 200:[0-9]+:conn-([0-9]+)\|200:[0-9]+:conn-([0-9]+)\|\ \;\ $ ]] \
   && [[ "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}" && "${BASH_REMATCH[3]}" == "${BASH_REMATCH[4]}" \
      && "${BASH_REMATCH[5]}" != "${BASH_REMATCH[6]}" && "${BASH_REMATCH[7]}" == "${BASH_REMATCH[8]}" ]]; then
    pass "client (get and streaming): CL + chunked is read as chunked on a connection not reused; chunked alone is reused (control)"
else
    fail "client CL + TE reuse (got '$RK'; want each CL+TE pair on two connections, each chunked pair on one)"
fi

# --- Streaming PUT (axl_http_request_streaming / _stream_file) ---
# UEFI client drives a multi-chunk PUT against the host's /upload
# endpoint, varying framing + producer source. Host records the
# received bytes; we GET /last-upload to verify size + head/tail
# bytes match the producer pattern (byte_i = i & 0xFF).

# Helper: hit the in-guest streaming-put driver, then read back
# what the host received. Args: mode, size.
SP_HOST_URL="http://10.0.2.2:${HOST_SERVER_PORT}/upload"
EXPECT_HEAD_HEX="0001020304050607"

# Content-Length framing, raw producer callback.
RESP=$(http_get "/streaming-put-test?url=${SP_HOST_URL}&size=2048&mode=cb")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "streaming-put cb: handler returns 200" \
                       || fail "streaming-put cb status (got $CODE)"
echo "$BODY" | grep -q '"rc":0' \
    && pass "streaming-put cb: rc 0 (request succeeded)" \
    || fail "streaming-put cb rc (body: $BODY)"
echo "$BODY" | grep -q '"server_status":201' \
    && pass "streaming-put cb: server returned 201" \
    || fail "streaming-put cb server_status (body: $BODY)"
HOST_RESP=$(curl -s "http://127.0.0.1:${HOST_SERVER_PORT}/last-upload")
echo "$HOST_RESP" | grep -q '"len": 2048' \
    && pass "streaming-put cb: host received 2048 bytes" \
    || fail "streaming-put cb host len (resp: $HOST_RESP)"
echo "$HOST_RESP" | grep -q "\"head_hex\": \"${EXPECT_HEAD_HEX}\"" \
    && pass "streaming-put cb: head bytes match producer pattern" \
    || fail "streaming-put cb head hex (resp: $HOST_RESP)"

# Transfer-Encoding: chunked framing, same producer. Verify tail
# bytes too — proves the reassembly worked across chunk boundaries
# (byte 4088..4095 of an i&0xFF pattern is f8..ff).
EXPECT_TAIL_HEX_4096="f8f9fafbfcfdfeff"
RESP=$(http_get "/streaming-put-test?url=${SP_HOST_URL}&size=4096&mode=chunked")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"rc":0' && echo "$BODY" | grep -q '"server_status":201' \
    && pass "streaming-put chunked: rc 0 + server 201" \
    || fail "streaming-put chunked outcome (body: $BODY)"
HOST_RESP=$(curl -s "http://127.0.0.1:${HOST_SERVER_PORT}/last-upload")
echo "$HOST_RESP" | grep -q '"len": 4096' \
    && pass "streaming-put chunked: host reassembled 4096 bytes" \
    || fail "streaming-put chunked host len (resp: $HOST_RESP)"
echo "$HOST_RESP" | grep -q "\"head_hex\": \"${EXPECT_HEAD_HEX}\"" \
    && pass "streaming-put chunked: head bytes match" \
    || fail "streaming-put chunked head hex (resp: $HOST_RESP)"
echo "$HOST_RESP" | grep -q "\"tail_hex\": \"${EXPECT_TAIL_HEX_4096}\"" \
    && pass "streaming-put chunked: tail bytes match (cross-chunk reassembly)" \
    || fail "streaming-put chunked tail hex (resp: $HOST_RESP)"

# Producer overshoots declared Content-Length → SDK detects + aborts.
# Pin the guard that prevents a buggy producer from sending more
# than total_size bytes when not using chunked transfer.
RESP=$(http_get "/streaming-put-test?url=${SP_HOST_URL}&size=2048&mode=overshoot")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"rc":0' \
    && fail "streaming-put overshoot: SDK accepted >Content-Length bytes (body: $BODY)" \
    || pass "streaming-put overshoot: SDK rejects producer overshoot"

# Producer returns AXL_ERR mid-stream → request fails, host doesn't
# see a complete body. We can't assert exactly what the host sees
# (race with the abort), but rc != 0 on the client side is the
# contract guarantee.
RESP=$(http_get "/streaming-put-test?url=${SP_HOST_URL}&size=4096&mode=err")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"rc":0' \
    && fail "streaming-put err: producer error masked as success (body: $BODY)" \
    || pass "streaming-put err: producer error surfaces as rc != 0"

# File-backed convenience wrapper. Producer comes from a UEFI file
# the handler writes first; SDK streams it via axl_fopen + axl_read.
RESP=$(http_get "/streaming-put-test?url=${SP_HOST_URL}&size=8192&mode=file")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"rc":0' && echo "$BODY" | grep -q '"server_status":201' \
    && pass "streaming-put file: rc 0 + server 201" \
    || fail "streaming-put file outcome (body: $BODY)"
HOST_RESP=$(curl -s "http://127.0.0.1:${HOST_SERVER_PORT}/last-upload")
echo "$HOST_RESP" | grep -q '"len": 8192' \
    && pass "streaming-put file: host received 8192 bytes" \
    || fail "streaming-put file host len (resp: $HOST_RESP)"
echo "$HOST_RESP" | grep -q "\"head_hex\": \"${EXPECT_HEAD_HEX}\"" \
    && pass "streaming-put file: head bytes match pattern from disk" \
    || fail "streaming-put file head hex (resp: $HOST_RESP)"

http_finish
