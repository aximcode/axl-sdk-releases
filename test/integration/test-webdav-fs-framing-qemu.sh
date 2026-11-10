#!/bin/bash
# test-meta: arch=x64 needs= est=75 local-only=0 guest=none
# guest=none: not converted yet (pool design §6.7 step 8 item 6): it reads
#   the boot disk's FAT image back with mtools.
# test-webdav-fs-framing-qemu.sh -- axl_http_server_serve_fs end to end, part 3: framing and names.
#
# The boot, the staging and the helpers are lib/webdav-fs-lib.sh, whose
# header says how the suite is split and why.
#
# Cases: chunked decoding and framing refusals (17b-17g), a live
#        temp reached by name (19), identity on axl_file_replace (20),
#        case-sensitive volumes (21), a part-way COPY (26), percent-decoding
#        (23), a large directory (24), a volume-root mount (25)
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# Usage: ./test/integration/test-webdav-fs-framing-qemu.sh [--arch X64|AARCH64]

WEBDAV_PART="framing"
source "$(dirname "$0")/lib/webdav-fs-lib.sh"

# What earlier cases left: precious.txt (23, 25), /dav/sub (23) and
# /dav/longdirectoryname (19a).
put /dav/precious.txt "precious v2"
code MKCOL /dav/sub >/dev/null
code MKCOL /dav/longdirectoryname >/dev/null

# 17b) A chunked PUT is DECODED, however its bytes arrive: whole with its
#      headers (the framing used to reach the file raw, and a terminator in
#      that first segment was never seen, so the PUT hung), and with a
#      chunk's size-line split across two segments (the upload path never
#      put the saved half back, so the rest was misread as the terminator
#      and an empty file was stored, 201).
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -H "Transfer-Encoding: chunked" \
    -T - "${BASE}/dav/chk1.txt" <<< "chunked one" 2>/dev/null || echo 000)
b=$(bodyz /dav/chk1.txt)
exec 4<>"/dev/tcp/127.0.0.1/${HOST_PORT}"
printf 'PUT /dav/chk2.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\nc\r' >&4
sleep 1
printf '\nchunked body\r\n0\r\n\r\n' >&4
st=$(timeout 10 head -1 <&4 | tr -d '\r' || true)
exec 4>&-
b2=$(bodyz /dav/chk2.txt)
[[ "$c" == 201 && "$b" == "chunked one" && "$st" == "HTTP/1.1 201 Created" && "$b2" == "chunked body" ]] \
    && pass "chunked PUTs decoded: body with the headers, and a size-line split across segments" \
    || fail "chunked decoding (with headers $c '$b', split size-line '$st' '$b2'; want 201 'chunked one' 201 'chunked body')"

# 17c) The chunk decoder can FAIL, and a failure is 400 with nothing
#      committed: a size line with no hex digits, one overflowing 64 bits,
#      one with a byte that is neither hex, ';' nor whitespace before its
#      CRLF, and one longer than the 1 KiB line bound. Each used to read as
#      the terminating chunk -- the body's prefix committed, 201.
# raw_send <port> <bytes...>: send each argument as its own segment (a
# second apart), then print the response's status line(s), one per line.
raw_send() {
    local port="$1"
    shift
    exec 4<>"/dev/tcp/127.0.0.1/${port}"
    local first=1
    for seg in "$@"; do
        [[ "$first" == 1 ]] || sleep 1
        first=0
        printf '%b' "$seg" >&4
    done
    if [[ -n "${RAW_FULL:-}" ]]; then
        timeout 10 cat <&4 | tr -d '\r' || true   # the whole response
    else
        timeout 10 cat <&4 | tr -d '\r' | grep -E '^HTTP/1\.1 ' || true
    fi
    exec 4>&-
}
PUTH='PUT /dav/chkbad.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'
put /dav/chkbad.txt "chkbad old"
s1=$(raw_send "$HOST_PORT" "${PUTH}5\r\nhello\r\nzz\r\nmore\r\n0\r\n\r\n")
s2=$(raw_send "$HOST_PORT" "${PUTH}5\r\nhello\r\n10000000000000005\r\nmore\r\n0\r\n\r\n")
s3=$(raw_send "$HOST_PORT" "${PUTH}5x\r\nhello\r\n0\r\n\r\n")
long=$(printf 'x%.0s' $(seq 1 1100))
s4=$(raw_send "$HOST_PORT" "${PUTH}5;${long}\r\nhello\r\n0\r\n\r\n")
s5=$(raw_send "$HOST_PORT" "${PUTH}\r\nhello\r\n0\r\n\r\n")
b=$(bodyz /dav/chkbad.txt)
[[ "$s1|$s2|$s3|$s4|$s5" == "HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request" \
   && "$b" == "chkbad old" ]] \
    && pass "malformed chunk size lines (no hex, 17 digits, junk, > 1 KiB, empty) -> 400, the file untouched" \
    || fail "malformed chunk sizes (no hex '$s1', overflow '$s2', junk '$s3', long '$s4', empty '$s5', file '$b'; want 400 x5 'chkbad old')"

# A size line with extensions, cut by a segment boundary inside them --
# past the 16 bytes the old decoder kept of a partial line -- is put back
# together; and trailers after the last chunk are read and ignored.
ext=$(printf 'e%.0s' $(seq 1 40))
s1=$(raw_send "$HOST_PORT" \
    'PUT /dav/chkext.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nc;name='"${ext:0:20}" \
    "${ext:20}"'\r\nchunked ext.\r\n0\r\nX-Trailer: yes\r\n\r\n')
b=$(bodyz /dav/chkext.txt)
[[ "$s1" == "HTTP/1.1 201 Created" && "$b" == "chunked ext." ]] \
    && pass "a size line with extensions split across segments, and trailers -> 201, the body whole" \
    || fail "split extension (status '$s1', body '$b'; want 201 'chunked ext.')"

# The final CRLF is consumed even when it arrives in the next segment, so
# the keep-alive connection's next request starts clean (it once began
# with that CRLF and was 400). The client sends the next request only
# after the PUT's response, as an HTTP/1.1 client that does not pipeline
# does.
s1=$(raw_send "$HOST_PORT" \
    'PUT /dav/chkka.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nka\r\n0\r\n' \
    '\r\n' \
    'GET /dav/chkka.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | tr '\n' '|')
[[ "$s1" == "HTTP/1.1 201 Created|HTTP/1.1 200 OK|" ]] \
    && pass "a terminator split before its last CRLF: the keep-alive request after it -> 201 then 200" \
    || fail "split terminator on keep-alive (statuses '$s1'; want 201 then 200)"

# A whole-body route decodes the same way: a malformed size line is 400,
# not the empty body dispatched.
s1=$(raw_send "$HOST_PORT" \
    'POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\nzz\r\n0\r\n\r\n')
[[ "$s1" == "HTTP/1.1 400 Bad Request" ]] \
    && pass "a whole-body chunked POST with a malformed size line -> 400" \
    || fail "whole-body malformed chunk (status '$s1'; want 400)"
# ...and a chunked body past the server's 4 MiB body limit is 413 -- it
# was cut at the limit and the cut body dispatched.
head -c 5000000 /dev/zero > "$TEST_TMPDIR/over.bin"
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X POST -H "Transfer-Encoding: chunked" \
    --data-binary @"$TEST_TMPDIR/over.bin" "${BASE}/echo" 2>/dev/null || true)
[[ "$c" == 413 ]] \
    && pass "a whole-body chunked POST over the body limit -> 413, not a cut body" \
    || fail "chunked body over the limit (got '$c'; want 413)"

# On an IN_PLACE mount the malformed line ABORTS the PUT. A body still in
# the server's 64 KiB upload buffer never reached write_open, so the file
# is untouched; one past it was written in place, and the abort's discard
# reports it partially written -- with the exact byte count the file holds.
put /dav/chkip.txt "chkip old"
s1=$(raw_send "$HOST_PORT4" \
    'PUT /ipa/chkip.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\nzz\r\n0\r\n\r\n')
b=$(bodyz /dav/chkip.txt)
big=$(head -c 70000 /dev/zero | tr '\0' 'a')
full=$(RAW_FULL=1 raw_send "$HOST_PORT4" \
    'PUT /ipa/chkip.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n11170\r\n'"${big}"'\r\nzz\r\n0\r\n\r\n')
s2=$(head -1 <<< "$full")
s2b=$(tail -1 <<< "$full")
sz=$(curl "${C[@]}" -o /dev/null -w '%{size_download}' "${BASE}/dav/chkip.txt" 2>/dev/null || echo 0)
test_clean_log
lg=$(grep -oE "PUT in place: 'FS0:.davroot.chkip\.txt' is partially written \([0-9]+ bytes of a chunked body\); its old content is gone" "$TEST_CLEAN_LOG" || true)
ln=$(grep -oE '\([0-9]+ bytes' <<< "$lg" | tr -dc '0-9' || true)
[[ "$s1" == "HTTP/1.1 400 Bad Request" && "$b" == "chkip old" && "$s2" == "HTTP/1.1 400 Bad Request" \
   && "$s2b" == "PUT failed part-way: the resource is partially written" \
   && -n "$ln" && "$ln" == "$sz" && "$sz" -gt 0 ]] \
    && pass "IN_PLACE + malformed size line -> 400; before any write the file is intact, after one the 400 and the log say partially written ($sz bytes)" \
    || fail "IN_PLACE malformed chunk (small '$s1' file '$b', large '$s2' body '$s2b', logged '$lg', file size $sz; want 400 'chkip old' 400 'PUT failed part-way: ...', the logged count = the size)"

# 17d) A Content-Length that is not a number, is empty, or does not fit
#      64 bits (or is the all-ones value the framing uses as "no body") is
#      400 before any handler runs -- it once read as 0 (an empty PUT
#      committed over the file) or wrapped.
put /dav/clbad.txt "clbad old"
CLH='PUT /dav/clbad.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: '
c1=$(raw_send "$HOST_PORT" "${CLH}abc\r\n\r\nxyz")
c2=$(raw_send "$HOST_PORT" "${CLH}\r\n\r\nxyz")
c3=$(raw_send "$HOST_PORT" "${CLH}18446744073709551617\r\n\r\nxyz")
c4=$(raw_send "$HOST_PORT" "${CLH}18446744073709551615\r\n\r\nxyz")
c5=$(raw_send "$HOST_PORT" "${CLH}3x\r\n\r\nxyz")
b=$(bodyz /dav/clbad.txt)
[[ "$c1|$c2|$c3|$c4|$c5" == "HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request" \
   && "$b" == "clbad old" ]] \
    && pass "a malformed or overflowing Content-Length -> 400, the file untouched" \
    || fail "bad Content-Length (abc '$c1', empty '$c2', 2^64+1 '$c3', 2^64-1 '$c4', '3x' '$c5', file '$b'; want 400 x5 'clbad old')"

# 17e) A header field value is read without its optional whitespace (RFC
#      9110 5.5) -- leading or trailing spaces and tabs -- so a
#      Content-Length of "5 ", "\t5" or " \t5 \t" is 5 (strict parsing once
#      made the trailing space a 400), and "chunked " is chunked.
# owsh <n>: a PUT of /dav/ows<n>.txt up to its last header (raw_send's %b
# turns the backslash escapes into bytes; no command substitution, which
# would eat the trailing newline).
owsh() { echo -n "PUT /dav/ows$1.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"; }
o1=$(raw_send "$HOST_PORT" "$(owsh 1)Content-Length: 5 \r\n\r\nhello")
o2=$(raw_send "$HOST_PORT" "$(owsh 2)Content-Length:\t5\r\n\r\nhello")
o3=$(raw_send "$HOST_PORT" "$(owsh 3)Content-Length: \t5 \t\r\n\r\nhello")
o4=$(raw_send "$HOST_PORT" "$(owsh 4)Transfer-Encoding: chunked \r\n\r\n5\r\nhello\r\n0\r\n\r\n")
b1=$(bodyz /dav/ows1.txt); b2=$(bodyz /dav/ows2.txt); b3=$(bodyz /dav/ows3.txt); b4=$(bodyz /dav/ows4.txt)
[[ "$o1|$o2|$o3|$o4" == "HTTP/1.1 201 Created|HTTP/1.1 201 Created|HTTP/1.1 201 Created|HTTP/1.1 201 Created" \
   && "$b1 $b2 $b3 $b4" == "hello hello hello hello" ]] \
    && pass "header values lose their OWS: Content-Length '5 ', tab-5, ' tab 5 tab', 'chunked ' -> 201 'hello'" \
    || fail "header OWS (trailing '$o1' '$b1', leading tab '$o2' '$b2', both '$o3' '$b3', chunked '$o4' '$b4'; want 201 'hello' x4)"

# 17f) Requests PIPELINED in one write on a keep-alive connection are all
#      answered, exactly and in order: bytes that arrive after a finished
#      request begin the next one, never dropped -- after a request with no
#      body, after a Content-Length PUT (whose body must not swallow them),
#      and after a chunked PUT (whose decoder stops at its terminator).
put /dav/pipe1.txt "first"
put /dav/pipe2.txt "second"
H='Host: 127.0.0.1\r\n'
p1=$(RAW_FULL=1 raw_send "$HOST_PORT" \
    "GET /dav/pipe1.txt HTTP/1.1\r\n${H}\r\nGET /dav/pipe2.txt HTTP/1.1\r\n${H}Connection: close\r\n\r\n" \
    | grep -E '^(HTTP/1\.1 |first|second)' | tr '\n' '|')
p2=$(RAW_FULL=1 raw_send "$HOST_PORT" \
    "PUT /dav/pipe3.txt HTTP/1.1\r\n${H}Content-Length: 5\r\n\r\npipedGET /dav/pipe3.txt HTTP/1.1\r\n${H}Connection: close\r\n\r\n" \
    | grep -E '^(HTTP/1\.1 |piped)' | tr '\n' '|')
p3=$(RAW_FULL=1 raw_send "$HOST_PORT" \
    "PUT /dav/pipe4.txt HTTP/1.1\r\n${H}Transfer-Encoding: chunked\r\n\r\n4\r\nchnk\r\n0\r\n\r\nGET /dav/pipe4.txt HTTP/1.1\r\n${H}Connection: close\r\n\r\n" \
    | grep -E '^(HTTP/1\.1 |chnk)' | tr '\n' '|')
# The Content-Length PUT again, its body split across two segments: the
# second segment holds the end of the body AND the next request, so the
# upload's own read (not the header buffer) must stop at the length.
p4=$(RAW_FULL=1 raw_send "$HOST_PORT" \
    "PUT /dav/pipe5.txt HTTP/1.1\r\n${H}Content-Length: 10\r\n\r\nsplit" \
    "-bodyGET /dav/pipe5.txt HTTP/1.1\r\n${H}Connection: close\r\n\r\n" \
    | grep -E '^(HTTP/1\.1 |split)' | tr '\n' '|')
b5=$(bodyz /dav/pipe5.txt)
b3=$(bodyz /dav/pipe3.txt)
[[ "$p4" == "HTTP/1.1 201 Created|HTTP/1.1 200 OK|split-body|" && "$b5" == "split-body" \
   && "$p1" == "HTTP/1.1 200 OK|first|HTTP/1.1 200 OK|second|" \
   && "$p2" == "HTTP/1.1 201 Created|HTTP/1.1 200 OK|piped|" \
   && "$p3" == "HTTP/1.1 201 Created|HTTP/1.1 200 OK|chnk|" && "$b3" == "piped" ]] \
    && pass "pipelined requests in one write: GET+GET, PUT(Content-Length)+GET, PUT(chunked)+GET all answered in order" \
    || fail "pipelining (GET+GET '$p1', CL PUT+GET '$p2', chunked PUT+GET '$p3', pipe3 '$b3', split CL PUT+GET '$p4' pipe5 '$b5'; want 200 first 200 second | 201 200 piped | 201 200 chnk, 'piped', 201 200 split-body, 'split-body')"

# 17g) Framing the server cannot be sure of is refused, never guessed
#      (RFC 9112), now that bytes past a request are read as the next one:
#      a Transfer-Encoding whose final coding is not chunked, or that
#      applies chunked twice, or is empty, is 400; one with a coding the
#      server does not implement before a final chunked is 501; whitespace
#      between a field name and its colon is 400 (5.1); an obs-fold line is
#      400 (5.2); a line with no colon is 400; a second Content-Length
#      or Transfer-Encoding line is 400. The target is untouched.
#      Content-Length together with Transfer-Encoding: chunked wins, and
#      the connection is closed after the response (6.3), so a request
#      pipelined behind it is not read.
put /dav/smug.txt "smug old"
SH='PUT /dav/smug.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n'
CB='\r\n\r\n5\r\nhello\r\n0\r\n\r\n'
t1=$(raw_send "$HOST_PORT" "${SH}Transfer-Encoding: gzip, chunked${CB}")
t2=$(raw_send "$HOST_PORT" "${SH}Transfer-Encoding: chunked, gzip${CB}")
t3=$(raw_send "$HOST_PORT" "${SH}Transfer-Encoding: gzip${CB}")
t4=$(raw_send "$HOST_PORT" "${SH}Transfer-Encoding: chunked, chunked${CB}")
t5=$(raw_send "$HOST_PORT" "${SH}Transfer-Encoding: ${CB}")
h1=$(raw_send "$HOST_PORT" "${SH}Content-Length : 5\r\n\r\nhello")
h2=$(raw_send "$HOST_PORT" "${SH}X-Fold: one\r\n  two\r\nContent-Length: 5\r\n\r\nhello")
h3=$(raw_send "$HOST_PORT" "${SH}NoColonHere\r\nContent-Length: 5\r\n\r\nhello")
h4=$(raw_send "$HOST_PORT" "${SH}Content-Length: 5\r\nContent-Length: 3\r\n\r\nhello")
h5=$(raw_send "$HOST_PORT" "${SH}Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked${CB}")
b=$(bodyz /dav/smug.txt)
[[ "$t1 $t2 $t3 $t4 $t5" == "HTTP/1.1 501 Not Implemented HTTP/1.1 400 Bad Request HTTP/1.1 400 Bad Request HTTP/1.1 400 Bad Request HTTP/1.1 400 Bad Request" \
   && "$h1|$h2|$h3|$h4|$h5" == "HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request" \
   && "$b" == "smug old" ]] \
    && pass "unsure framing refused: TE gzip,chunked 501; chunked,gzip / gzip / chunked,chunked / empty 400; 'name :' 400; obs-fold 400; no colon 400; a second CL or TE line 400; file untouched" \
    || fail "framing refusals (TE gzip,chunked '$t1', chunked,gzip '$t2', gzip '$t3', chunked,chunked '$t4', empty '$t5'; 'name :' '$h1', obs-fold '$h2', no colon '$h3', two CL '$h4', two TE '$h5'; file '$b'; want 501, 400 x9, 'smug old')"
# A bare CR once ended header parsing silently: what followed it --
# a Content-Length, a Transfer-Encoding, a second Content-Length -- was
# never read, the body was taken for the next request, and the checker
# (splitting at CRLF only) passed it. Each is now 400 with ONE response
# (the pipelined GET behind it is not read) and the file untouched; so are
# a name that is not a token, a NUL in a name, and a bare CR or a tab in
# the request line.
SMH='PUT /dav/smug.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n'
GB='GET /dav/smug.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'
r1=$(raw_send "$HOST_PORT" "${SMH}X: a\rContent-Length: 5\r\n\r\nhello${GB}" | tr '\n' '|')
r2=$(raw_send "$HOST_PORT" "${SMH}X: a\rTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n${GB}" | tr '\n' '|')
r3=$(raw_send "$HOST_PORT" "${SMH}Content-Length: 5\rContent-Length: 50\r\n\r\nhello${GB}" | tr '\n' '|')
r4=$(raw_send "$HOST_PORT" "${SMH}Content(Length: 5\r\n\r\nhello${GB}" | tr '\n' '|')
r5=$(raw_send "$HOST_PORT" "${SMH}Con\0tent-Length: 5\r\n\r\nhello${GB}" | tr '\n' '|')
r6=$(raw_send "$HOST_PORT" "PUT /dav/smug.txt\r HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\n\r\nhello" | tr '\n' '|')
r7=$(raw_send "$HOST_PORT" "PUT\t/dav/smug.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\n\r\nhello" | tr '\n' '|')
b=$(bodyz /dav/smug.txt)
BR='HTTP/1.1 400 Bad Request|'
[[ "$r1$r2$r3$r4$r5$r6$r7" == "$BR$BR$BR$BR$BR$BR$BR" && "$b" == "smug old" ]] \
    && pass "a bare CR before CL / before TE / between two CLs, a non-token name, a NUL in a name, a CR or tab in the request line -> one 400 each, file untouched" \
    || fail "bare CR and control characters (CL '$r1', TE '$r2', dup CL '$r3', 'Content(Length' '$r4', NUL name '$r5', CR request line '$r6', tab request line '$r7', file '$b'; want one 400 each, 'smug old')"
c1=$(RAW_FULL=1 raw_send "$HOST_PORT" \
    'PUT /dav/smug2.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\nGET /dav/smug.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' \
    | grep -E '^HTTP/1\.1 ' | tr '\n' '|')
b2=$(bodyz /dav/smug2.txt)
[[ "$c1" == "HTTP/1.1 201 Created|" && "$b2" == "hello" ]] \
    && pass "Content-Length with Transfer-Encoding: chunked framing wins, the connection closes after the response" \
    || fail "CL + TE (statuses '$c1', body '$b2'; want only 201, 'hello')"

# HTTP/1.0 with Transfer-Encoding is faulty framing (RFC 9112 6.1): the
# body is read as chunked and the connection closes after the response,
# so a request pipelined behind it is not read.
c1=$(RAW_FULL=1 raw_send "$HOST_PORT" \
    'PUT /dav/smug3.txt HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\nGET /dav/smug.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' \
    | grep -E '^HTTP/1\.1 ' | tr '\n' '|')
b3=$(bodyz /dav/smug3.txt)
[[ "$c1" == "HTTP/1.1 201 Created|" && "$b3" == "hello" ]] \
    && pass "HTTP/1.0 with Transfer-Encoding: read as chunked, the connection closes after the response" \
    || fail "HTTP/1.0 + TE (statuses '$c1', body '$b3'; want only 201, 'hello')"

# Host (RFC 9112 3.2): an HTTP/1.1 request without one is 400, and any
# request with two is 400; HTTP/1.0 without one is answered.
hn=$(raw_send "$HOST_PORT" 'GET /dav/smug.txt HTTP/1.1\r\nConnection: close\r\n\r\n')
hd=$(raw_send "$HOST_PORT" 'GET /dav/smug.txt HTTP/1.1\r\nHost: a\r\nHost: b\r\nConnection: close\r\n\r\n')
h0=$(raw_send "$HOST_PORT" 'GET /dav/smug.txt HTTP/1.0\r\nConnection: close\r\n\r\n')
[[ "$hn|$hd|$h0" == "HTTP/1.1 400 Bad Request|HTTP/1.1 400 Bad Request|HTTP/1.1 200 OK" ]] \
    && pass "Host: HTTP/1.1 without one 400, two of them 400, HTTP/1.0 without one 200" \
    || fail "Host rules (1.1 no Host '$hn', two Hosts '$hd', 1.0 no Host '$h0'; want 400 400 200)"

# The request line is exactly three parts, the last HTTP/DIGIT.DIGIT (RFC
# 9112 3): a fourth part, four parts, a target that only ends like a
# version, lowercase http and a missing version are 400 -- the version was
# once read from the line's last 8 bytes, so each was served. A major
# version other than 1 is 505 (RFC 9110 15.6.6). Controls: HTTP/1.1 and
# HTTP/1.0 are answered.
HH='\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'
v1=$(raw_send "$HOST_PORT" "GET /dav/smug.txt HTTP/1.1 junk${HH}")
v2=$(raw_send "$HOST_PORT" "GET /a b c d${HH}")
v3=$(raw_send "$HOST_PORT" "GET /HTTP/1.0${HH}")
v4=$(raw_send "$HOST_PORT" "GET /dav/smug.txt HTTP/2.0${HH}")
v5=$(raw_send "$HOST_PORT" "GET /dav/smug.txt http/1.1${HH}")
v6=$(raw_send "$HOST_PORT" "GET /dav/smug.txt${HH}")
v7=$(raw_send "$HOST_PORT" "GET /dav/smug.txt HTTP/1.1${HH}")
v8=$(raw_send "$HOST_PORT" "GET /dav/smug.txt HTTP/1.0${HH}")
B4='HTTP/1.1 400 Bad Request'
[[ "$v1|$v2|$v3|$v4|$v5|$v6|$v7|$v8" == "$B4|$B4|$B4|HTTP/1.1 505 HTTP Version Not Supported|$B4|$B4|HTTP/1.1 200 OK|HTTP/1.1 200 OK" ]] \
    && pass "request line: a 4th part, 4 parts, '/HTTP/1.0' as target, lowercase http, no version 400; HTTP/2.0 505; 1.1 and 1.0 200" \
    || fail "request line (junk '$v1', 4 parts '$v2', /HTTP/1.0 '$v3', HTTP/2.0 '$v4', http/1.1 '$v5', no version '$v6', 1.1 '$v7', 1.0 '$v8'; want 400 x3, 505, 400 x2, 200, 200)"

# 19) A live temp is matched by its NAME wherever a path reaches it -- its
#     8.3 name is process-unique and has no alias of its own, but the
#     directory it sits in can be spelled another way.
# 19a) Through the parent directory's 8.3 alias: not listed, not served,
#      not written; and the dropped PUT removes only its own temp.
raw_put_hold "$HOST_PORT" /dav/longdirectoryname/pa.txt 1048576 200000
put_temps 1 d=longdirectoryname || true
tmpn=$(fs_ls d=longdirectoryname | grep -E "$TEMP_RE" || true)
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/LONGDI~1/" 2>/dev/null || true)
listed=$(grep -ci -- "${tmpn:-NO-TEMP}" <<< "$r" || true)
cg=$(code GET "/dav/LONGDI~1/${tmpn}")
cp=$(putc "/nodel/LONGDI~1/${tmpn}" "x")
raw_put_drop
put_temps 0 d=longdirectoryname || true
left=$(fs_ls d=longdirectoryname | grep -E "$TEMP_RE" | tr '\n' ' ' || true)
[[ "$tmpn" =~ $TEMP_RE && "$listed" == 0 && "$cg" == 404 && "$cp" == 403 && -z "$left" ]] \
    && pass "a live temp through its parent's 8.3 alias: not listed, GET 404, PUT 403, nothing left" \
    || fail "parent alias (temp '$tmpn', listed $listed, GET $cg, PUT $cp, left '$left'; want ~APxxxxx.TMP 0 404 403 '')"

# 19b) A PUT whose TARGET becomes another PUT's live temp. Temp numbers
#      run in sequence, so one dropped PUT shows the next two: A targets the
#      name B's temp will take, B starts after A, and A's commit must not
#      replace B's temp (403) -- B still stores its body.
raw_put_hold "$HOST_PORT" /dav/i1probe.txt 1048576 200000
put_temps 1 || true
n0=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' || true)
raw_put_drop
put_temps 0 || true
ta=""
[[ "$n0" =~ $TEMP_RE ]] && ta=$(printf '~AP%05X.TMP' $(( (16#${n0:3:5} + 2) & 0xFFFFF )))
raw_put_hold "$HOST_PORT" "/dav/${ta:-none}" 300000 200000
put_temps 1 || true
RAW_FD=5 raw_put_hold "$HOST_PORT" /nodel/i1b.txt 300000 200000
put_temps 2 || true
isb=$(fs_ls | grep -cxF -- "${ta:-none}" || true)
sta=$(raw_put_finish 100000)
stb=$(RAW_FD=5 raw_put_finish 100000)
szb=$(curl "${C[@]}" -o /dev/null -w '%{size_download}' "${BASE}/dav/i1b.txt" 2>/dev/null || true)
ga=$(code GET "/dav/${ta:-none}")
[[ -n "$ta" && "$isb" == 1 && "$sta" == "HTTP/1.1 403 Forbidden" && "$stb" == "HTTP/1.1 201 Created" \
   && "$szb" == 300000 && "$ga" == 404 ]] \
    && pass "a PUT whose target is another PUT's live temp -> 403; that PUT stores its body" \
    || fail "target = live temp (A's target '$ta', B's temp there $isb, A '$sta', B '$stb', B size $szb, A's target GET $ga; want 1 403 201 300000 404)"

# 19c) A Depth-infinity COPY of a directory holding a live temp does not
#      copy the temp.
code MKCOL /dav/cpp >/dev/null
raw_put_hold "$HOST_PORT" /dav/cpp/f.txt 1048576 200000
put_temps 1 d=cpp || true
cc=$(code COPY /dav/cpp -H "Destination: ${BASE}/dav/cpp2" -H "Depth: infinity")
copied=$(fs_ls d=cpp2 | grep -E "$TEMP_RE" | tr '\n' ' ' || true)
raw_put_drop
put_temps 0 d=cpp || true
[[ "$cc" == 201 && -z "$copied" ]] \
    && pass "a COPY of a directory holding a live temp does not copy it" \
    || fail "COPY during a PUT (got $cc, temps in the copy '$copied'; want 201 '')"

# 20) MOVE and COPY on axl_file_replace: identity by the names the
#     filesystem stores, refusals before anything changes.
# 20a) A MOVE that only changes the name's letter case -- how clients that
#      rename by MOVE rename a.txt to A.TXT, with either Overwrite value --
#      renames it: 204. The destination "exists" on case-insensitive FAT only
#      because it IS the source, so Overwrite: F does not refuse it.
put /dav/case.txt "case"
c=$(code MOVE /dav/case.txt -H "Destination: ${BASE}/dav/CASE.TXT")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
b=$(body /dav/CASE.TXT)
[[ "$c" == 204 && "$r" == *"/dav/CASE.TXT<"* && "$r" != *"/dav/case.txt<"* && "$b" == "case" ]] \
    && pass "a case-only MOVE renames: 204, listed as CASE.TXT" \
    || fail "case-only MOVE (got $c, listed CASE.TXT $([[ "$r" == *"/dav/CASE.TXT<"* ]] && echo yes || echo no), case.txt $([[ "$r" == *"/dav/case.txt<"* ]] && echo yes || echo no), body '$b'; want 204 yes no 'case')"

put /dav/case2.txt "case2"
c=$(code MOVE /dav/case2.txt -H "Destination: ${BASE}/dav/CASE2.TXT" -H "Overwrite: F")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
b=$(body /dav/CASE2.TXT)
[[ "$c" == 204 && "$r" == *"/dav/CASE2.TXT<"* && "$r" != *"/dav/case2.txt<"* && "$b" == "case2" ]] \
    && pass "a case-only MOVE with Overwrite: F renames too: 204, listed as CASE2.TXT" \
    || fail "case-only MOVE, Overwrite F (got $c, listed CASE2.TXT $([[ "$r" == *"/dav/CASE2.TXT<"* ]] && echo yes || echo no), case2.txt $([[ "$r" == *"/dav/case2.txt<"* ]] && echo yes || echo no), body '$b'; want 204 yes no 'case2')"

# A trailing '/' on one side only does not change that: a collection
# renamed by case, casedir/ -> CASEDIR, with Overwrite: F, is 204 too.
code MKCOL /dav/casedir >/dev/null
c=$(code MOVE /dav/casedir/ -H "Destination: ${BASE}/dav/CASEDIR" -H "Overwrite: F")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
[[ "$c" == 204 && "$r" == *"/dav/CASEDIR/<"* && "$r" != *"/dav/casedir/<"* ]] \
    && pass "a case-only MOVE of a collection, slash on one side, Overwrite: F -> 204, listed as CASEDIR" \
    || fail "case-only collection MOVE (got $c, CASEDIR listed $([[ "$r" == *"/dav/CASEDIR/<"* ]] && echo yes || echo no), casedir $([[ "$r" == *"/dav/casedir/<"* ]] && echo yes || echo no); want 204 yes no)"

# A COPY onto the source's own case variant is a copy onto itself on FAT:
# 403 with Overwrite: F too (the case variant reaches the op), not 412.
put /dav/copycase.txt "cc"
c=$(code COPY /dav/copycase.txt -H "Destination: ${BASE}/dav/COPYCASE.TXT" -H "Overwrite: F")
b=$(body /dav/copycase.txt)
[[ "$c" == 403 && "$b" == "cc" ]] \
    && pass "a COPY onto the source's case variant, Overwrite: F, on FAT -> 403, intact" \
    || fail "case-variant COPY, Overwrite F (got $c, body '$b'; want 403 'cc')"

# 20a3) A PUT addressed to an existing file's 8.3 alias replaces the file and
#       keeps its long name: the resource is not renamed to the alias.
put /dav/aliasputtarget.txt "v1"
c=$(putc /dav/ALIASP~1.TXT "v2")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
b=$(body /dav/aliasputtarget.txt)
[[ "$c" == 201 && "$b" == "v2" && "$r" == *"/dav/aliasputtarget.txt<"* && "$r" != *"/dav/ALIASP~1.TXT<"* ]] \
    && pass "a PUT through a file's 8.3 alias replaces it under its long name" \
    || fail "PUT through an alias (got $c, body '$b', long name listed $([[ "$r" == *"/dav/aliasputtarget.txt<"* ]] && echo yes || echo no), alias listed $([[ "$r" == *"/dav/ALIASP~1.TXT<"* ]] && echo yes || echo no); want 201 'v2' yes no)"

# 20b) A MOVE onto the 8.3 alias of the source itself names one entry: 403,
#      nothing changed.
put /dav/longfilename.txt "lf"
c=$(code MOVE /dav/longfilename.txt -H "Destination: ${BASE}/dav/LONGFI~1.TXT")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
b=$(body /dav/longfilename.txt)
[[ "$c" == 403 && "$b" == "lf" && "$r" == *"/dav/longfilename.txt<"* ]] \
    && pass "a MOVE onto the source's own 8.3 alias -> 403, nothing changed" \
    || fail "MOVE onto the source's alias (got $c, body '$b', still listed $([[ "$r" == *"/dav/longfilename.txt<"* ]] && echo yes || echo no); want 403 'lf' yes)"

# 20c) A COPY of a collection into its own subtree is refused before any
#      copying: 403, nothing created.
code MKCOL /dav/cself >/dev/null
put /dav/cself/x.txt "x"
c=$(code COPY /dav/cself -H "Destination: ${BASE}/dav/cself/sub" -H "Depth: infinity")
cs=$(code PROPFIND /dav/cself/sub -H "Depth: 0")
l=$(fs_ls d=cself | tr '\n' ' ')
[[ "$c" == 403 && "$cs" == 404 && "$l" == "x.txt " ]] \
    && pass "a COPY of a collection into its own subtree -> 403, nothing created" \
    || fail "COPY into its own subtree (got $c, sub $cs, listing '$l'; want 403 404 'x.txt ')"

# 20e) A COPY keeps its source: a COPY onto the source's own 8.3 alias is
#      a copy onto itself -- 403, the file intact (it used to be truncated
#      by the copy's own write) -- and a read-only source copies (201).
c=$(code COPY /dav/longfilename.txt -H "Destination: ${BASE}/dav/LONGFI~1.TXT")
b=$(body /dav/longfilename.txt)
c2=$(code COPY /dav/rofile.txt -H "Destination: ${BASE}/dav/rocopy.txt")
b2=$(body /dav/rocopy.txt)
[[ "$c" == 403 && "$b" == "lf" && "$c2" == 201 && "$b2" == "read only" ]] \
    && pass "COPY onto the source's alias -> 403 intact; COPY of a read-only file -> 201" \
    || fail "COPY keeps its source (onto alias $c '$b', read-only source $c2 '$b2'; want 403 'lf' 201 'read only')"

# 20d) A MOVE on a volume with no room for one more directory entry is
#      refused before it changes anything: 507, both names as they were.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Destination: ${BASE2}/full/b.txt" "${BASE2}/full/a.txt" 2>/dev/null || echo 000)
ba=$(bodyz /full/a.txt "$BASE2")
cb=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' "${BASE2}/full/b.txt" 2>/dev/null || echo 000)
[[ "$c" == 507 && "$ba" == "a" && "$cb" == 404 ]] \
    && pass "a MOVE on a full volume -> 507, both sides intact" \
    || fail "MOVE on a full volume (got $c, a.txt '$ba', b.txt GET $cb; want 507 'a' 404)"

# 20f) A PUT whose commit finds the volume without room for the new name
#      is 507, nothing left behind: an empty body onto the full volume makes
#      its temp (a FAT16 root has fixed slots, and an empty file holds no
#      cluster) and then cannot commit.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X PUT -H "Content-Length: 0" \
    --data-binary "" "${BASE2}/full/new.txt" 2>/dev/null || echo 000)
cg=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' "${BASE2}/full/new.txt" 2>/dev/null || echo 000)
l=$(fs_ls full | tr '\n' ' ')
[[ "$c" == 507 && "$cg" == 404 && "$l" == "a.txt filler.bin " ]] \
    && pass "a PUT whose commit finds no room -> 507, no file, no temp" \
    || fail "PUT on a full volume (got $c, GET $cg, listing '$l'; want 507 404 'a.txt filler.bin ')"

# 20g) A Depth: 0 COPY of a collection copies the collection only, so its
#      room is counted for the collection only: on a volume with two free
#      clusters (8081 /near) it is 201 where a Depth: infinity COPY of the
#      same collection, with a 16 KiB file below it, is 507.
cd=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X COPY -H "Depth: infinity" \
    -H "Destination: ${BASE2}/near/tree1" "${BASE2}/near/tree" 2>/dev/null || echo 000)
c0=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X COPY -H "Depth: 0" \
    -H "Destination: ${BASE2}/near/tree0" "${BASE2}/near/tree" 2>/dev/null || echo 000)
l=$(fs_ls near | tr '\n' ' ')
[[ "$cd" == 507 && "$c0" == 201 && "$l" == "tree filler.bin tree0 " ]] \
    && pass "near-full volume: a deep COPY -> 507, a Depth: 0 COPY -> 201" \
    || fail "COPY room by depth (deep $cd, Depth 0 $c0, listing '$l'; want 507 201 'tree filler.bin tree0 ')"

# 21) On a case-SENSITIVE volume (8082, the provider fixture) a.txt and
#     A.txt are two entries, so a MOVE or COPY between them is an overwrite
#     like any other -- decided by identity, never by spelling: Overwrite: F
#     is 412 and both stay; on a no-delete mount a collection is not
#     replaced by one whose name differs only in case: 403, both intact.
c1=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Overwrite: F" \
    -H "Destination: ${BASE3}/cs/A.txt" "${BASE3}/cs/a.txt" 2>/dev/null || echo 000)
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X COPY -H "Overwrite: F" \
    -H "Destination: ${BASE3}/cs/A.txt" "${BASE3}/cs/a.txt" 2>/dev/null || echo 000)
bl=$(bodyz /cs/a.txt "$BASE3")
bu=$(bodyz /cs/A.txt "$BASE3")
[[ "$c1" == 412 && "$c2" == 412 && "$bl" == "lower" && "$bu" == "UPPER" ]] \
    && pass "case-sensitive volume: MOVE and COPY a.txt -> A.txt, Overwrite F -> 412, both intact" \
    || fail "case-sensitive MOVE/COPY (got $c1 / $c2, a.txt '$bl', A.txt '$bu'; want 412 412 'lower' 'UPPER')"
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Overwrite: T" \
    -H "Destination: ${BASE3}/csnd/DIR/" "${BASE3}/csnd/dir/" 2>/dev/null || echo 000)
pl=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X PROPFIND -H "Depth: 0" "${BASE3}/cs/dir" 2>/dev/null || echo 000)
pu=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X PROPFIND -H "Depth: 0" "${BASE3}/cs/DIR" 2>/dev/null || echo 000)
[[ "$c" == 403 && "$pl" == 207 && "$pu" == 207 ]] \
    && pass "case-sensitive, no-delete: MOVE dir -> DIR -> 403, both collections intact" \
    || fail "case-sensitive no-delete MOVE (got $c, dir $pl, DIR $pu; want 403 207 207)"

# 21b) The policy flags refuse ANOTHER entry only, never the source under
#      another spelling. On FAT: a collection renamed by case on the
#      no-delete mount is 204; on the no-overwrite mount (8081) with
#      Overwrite: T a file renamed by case is 204, and a MOVE onto the
#      source's own 8.3 alias is 403 (one resource), nothing changed.
code MKCOL /dav/ndcase >/dev/null
c1=$(code MOVE /nodel/ndcase -H "Destination: ${BASE}/nodel/NDCASE" -H "Overwrite: T")
put /dav/nocase.txt "nocase"
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Overwrite: T" \
    -H "Destination: ${BASE2}/dav/NOCASE.TXT" "${BASE2}/dav/nocase.txt" 2>/dev/null || echo 000)
put /dav/noaliasfile.txt "na"
c3=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Overwrite: T" \
    -H "Destination: ${BASE2}/dav/NOALIA~1.TXT" "${BASE2}/dav/noaliasfile.txt" 2>/dev/null || echo 000)
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
b3=$(body /dav/noaliasfile.txt)
[[ "$c1 $c2 $c3" == "204 204 403" && "$r" == *"/dav/NDCASE/<"* && "$r" == *"/dav/NOCASE.TXT<"* \
   && "$r" != *"/dav/nocase.txt<"* && "$b3" == "na" ]] \
    && pass "renames by case pass the policy (no-delete collection 204, no-overwrite file 204); a MOVE onto the alias is 403" \
    || fail "policy vs the source itself (no-delete case $c1, no-overwrite case $c2, alias $c3, NDCASE $([[ "$r" == *"/dav/NDCASE/<"* ]] && echo yes || echo no), NOCASE.TXT $([[ "$r" == *"/dav/NOCASE.TXT<"* ]] && echo yes || echo no), alias source '$b3'; want 204 204 403 yes yes 'na')"

# With Overwrite: F the same MOVE and a COPY onto the alias are 412: the
# handler finds the destination existing and answers before the op runs.
c1=$(code MOVE /dav/noaliasfile.txt -H "Destination: ${BASE}/dav/NOALIA~1.TXT" -H "Overwrite: F")
c2=$(code COPY /dav/noaliasfile.txt -H "Destination: ${BASE}/dav/NOALIA~1.TXT" -H "Overwrite: F")
b=$(body /dav/noaliasfile.txt)
[[ "$c1 $c2" == "412 412" && "$b" == "na" ]] \
    && pass "MOVE and COPY onto the source's alias with Overwrite: F -> 412, nothing changed" \
    || fail "alias with Overwrite F (MOVE $c1, COPY $c2, source '$b'; want 412 412 'na')"

# 21c) On the case-sensitive volume a case-variant transfer the policy
#      allows is an ordinary replace: MOVE a.txt -> A.txt with Overwrite: T
#      is 204 and A.txt holds a.txt's body; COPY dir -> DIR on the no-delete
#      mount is 403, both collections intact.
c1=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X COPY -H "Overwrite: T" \
    -H "Destination: ${BASE3}/csnd/DIR" "${BASE3}/csnd/dir" 2>/dev/null || echo 000)
pl=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X PROPFIND -H "Depth: 0" "${BASE3}/cs/dir" 2>/dev/null || echo 000)
pu=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X PROPFIND -H "Depth: 0" "${BASE3}/cs/DIR" 2>/dev/null || echo 000)
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Overwrite: T" \
    -H "Destination: ${BASE3}/cs/A.txt" "${BASE3}/cs/a.txt" 2>/dev/null || echo 000)
bu=$(bodyz /cs/A.txt "$BASE3")
gl=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' "${BASE3}/cs/a.txt" 2>/dev/null || echo 000)
[[ "$c1" == 403 && "$pl" == 207 && "$pu" == 207 && "$c2" == 204 && "$bu" == "lower" && "$gl" == 404 ]] \
    && pass "case-sensitive: COPY dir -> DIR (no-delete) 403 intact; MOVE a.txt -> A.txt, Overwrite T, 204 replaces" \
    || fail "case-sensitive allowed/refused (COPY $c1, dir $pl, DIR $pu, MOVE $c2, A.txt '$bu', a.txt GET $gl; want 403 207 207 204 'lower' 404)"

# 26) A COPY that fails PART-WAY removes what it made and puts back what it
#     replaced. pc/ holds a file 199 characters deep; copied onto a
#     60-character name every path below grows by 58 and the deepest no
#     longer fits FAT's path limit, so the copy fails after making the
#     levels above it: 500, the source whole, no partial copy, no aside --
#     and, over an existing collection (Overwrite: T), that collection back
#     with its file.
seg() { printf "%${2}s" "" | tr ' ' "$1"; }
P1=$(seg a 47); P2=$(seg b 47); P3=$(seg c 47); P4=$(seg d 46)
LONGN=$(seg x 60)
code MKCOL /dav/pc >/dev/null
code MKCOL "/dav/pc/$P1" >/dev/null
code MKCOL "/dav/pc/$P1/$P2" >/dev/null
code MKCOL "/dav/pc/$P1/$P2/$P3" >/dev/null
code MKCOL "/dav/pc/$P1/$P2/$P3/$P4" >/dev/null
DEEP="pc/$P1/$P2/$P3/$P4/f.txt"
put "/dav/$DEEP" "deep"
# The leftover aside 6c keeps on purpose is the baseline.
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
asides0=$(grep -cE '~AXL[0-9A-F]{4}\.TMP' <<< "$r" || true)
c=$(code COPY /dav/pc -H "Destination: ${BASE}/dav/${LONGN}" -H "Depth: infinity")
bd=$(body "/dav/$DEEP")
cl=$(code PROPFIND "/dav/${LONGN}" -H "Depth: 0")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
asides=$(grep -cE '~AXL[0-9A-F]{4}\.TMP' <<< "$r" || true)
[[ ${#DEEP} == 199 && "$c" == 500 && "$bd" == "deep" && "$cl" == 404 && "$asides" == "$asides0" ]] \
    && pass "a COPY that fails part-way (path ${#DEEP} -> $(( ${#DEEP} + 58 ))) -> 500, source whole, nothing left" \
    || fail "part-way COPY (deep path ${#DEEP}, COPY $c, source '$bd', destination $cl, asides $asides; want 199 500 'deep' 404 $asides0)"

code MKCOL "/dav/${LONGN}" >/dev/null
put "/dav/${LONGN}/keep.txt" "kept"
c=$(code COPY /dav/pc -H "Destination: ${BASE}/dav/${LONGN}" -H "Depth: infinity" -H "Overwrite: T")
bk=$(body "/dav/${LONGN}/keep.txt")
cp1=$(code PROPFIND "/dav/${LONGN}/$P1" -H "Depth: 0")
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
asides=$(grep -cE '~AXL[0-9A-F]{4}\.TMP' <<< "$r" || true)
[[ "$c" == 500 && "$bk" == "kept" && "$cp1" == 404 && "$asides" == "$asides0" ]] \
    && pass "a part-way COPY over an existing collection -> 500, that collection back as it was" \
    || fail "part-way COPY over a collection (COPY $c, its file '$bk', partial level $cp1, asides $asides; want 500 'kept' 404 $asides0)"

# 23) Request paths and Destination are percent-DECODED (RFC 3986 2.1) before
#     they are mapped, and PROPFIND hrefs percent-ENCODED. A name with a
#     space or a non-ASCII letter round-trips; an encoded separator (%2F,
#     %5C), a NUL (%00), a raw backslash, or a malformed escape is 400; and
#     "%2e%2e" is "..", resolved INSIDE the mount like any other -- never out
#     of it (FS0:\AxlTestNet.efi, one level above the mount, is the control
#     that something is there to reach).
c=$(putc "/dav/with%20space.txt" "spaced")
b=$(bodyz "/dav/with%20space.txt")
inls=$(fs_ls | grep -cxF "with space.txt" || true)
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
[[ "$c" == 201 && "$b" == "spaced" && "$inls" == 1 && "$r" == *"<D:href>/dav/with%20space.txt</D:href>"* \
   && "$r" == *"<D:displayname>with space.txt</D:displayname>"* ]] \
    && pass "a %20 path: stored as 'with space.txt', GET by it, href encoded" \
    || fail "%20 path (PUT $c, body '$b', on disk $inls, href/displayname $([[ "$r" == *"/dav/with%20space.txt<"* ]] && echo yes || echo no); want 201 'spaced' 1 yes)"

c=$(putc "/dav/%C3%A9t%C3%A9.txt" "summer")
b=$(bodyz "/dav/%C3%A9t%C3%A9.txt")
inls=$(fs_ls | grep -cxF "été.txt" || true)
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
[[ "$c" == 201 && "$b" == "summer" && "$inls" == 1 && "$r" == *"<D:href>/dav/%C3%A9t%C3%A9.txt</D:href>"* ]] \
    && pass "a UTF-8 path: stored as 'été.txt', GET by it, href encoded" \
    || fail "UTF-8 path (PUT $c, body '$b', on disk $inls, href $([[ "$r" == *"/dav/%C3%A9t%C3%A9.txt<"* ]] && echo yes || echo no); want 201 'summer' 1 yes)"

c=$(code MOVE "/dav/with%20space.txt" -H "Destination: ${BASE}/dav/moved%20here.txt")
b=$(bodyz "/dav/moved%20here.txt")
inls=$(fs_ls | grep -cxF "moved here.txt" || true)
[[ "$c" == 201 && "$b" == "spaced" && "$inls" == 1 ]] \
    && pass "MOVE with an encoded Destination -> 201, 'moved here.txt'" \
    || fail "encoded Destination (MOVE $c, body '$b', on disk $inls; want 201 'spaced' 1)"

g1=$(code GET "/dav/a%2Fb.txt"); g2=$(code GET "/dav/a%2fb.txt"); g3=$(code GET "/dav/a%5Cb.txt")
g4=$(code GET "/dav/a%00b.txt"); g5=$(code GET "/dav/a%zzb.txt"); g6=$(code GET "/dav/a%4")
p1=$(putc "/dav/x%2Fy.txt" "x")
m1=$(code MOVE "/dav/moved%20here.txt" -H "Destination: ${BASE}/dav/x%2Fy.txt")
bs=$(bodyz "/dav/moved%20here.txt")
[[ "$g1 $g2 $g3 $g4 $g5 $g6 $p1 $m1" == "400 400 400 400 400 400 400 400" && "$bs" == "spaced" ]] \
    && pass "%2F %2f %5C %00, a malformed escape, a cut one -> 400 (GET, PUT, MOVE Destination)" \
    || fail "forbidden escapes (GET $g1 $g2 $g3 $g4 $g5 $g6, PUT $p1, MOVE $m1, source '$bs'; want 400 x8 'spaced')"

# A raw backslash would be a UEFI separator after mapping -- "..\" inside
# one URL segment that the containment check never sees.
exec 4<>"/dev/tcp/127.0.0.1/${HOST_PORT}"
printf 'GET /dav/x\\..\\..\\AxlTestNet.efi HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' >&4
st=$(timeout 10 head -1 <&4 | tr -d '\r' || true)
exec 4>&-
e1=$(curl "${C[@]}" --path-as-is -o /dev/null -w '%{http_code}' "${BASE}/dav/%2e%2e/AxlTestNet.efi" 2>/dev/null || echo 000)
e2=$(curl "${C[@]}" --path-as-is -o /dev/null -w '%{http_code}' "${BASE}/dav/sub/%2E%2E/%2e%2e/AxlTestNet.efi" 2>/dev/null || echo 000)
e3=$(curl "${C[@]}" --path-as-is -o /dev/null -w '%{http_code}' "${BASE}/dav/sub/%2e%2e/precious.txt" 2>/dev/null || echo 000)
ctl=$(fs_ls d=.. | grep -cxiF "AxlTestNet.efi" || true)
[[ "$st" == "HTTP/1.1 400 Bad Request" && "$e1" == 404 && "$e2" == 404 && "$e3" == 200 && "$ctl" == 1 ]] \
    && pass "no escape: raw backslash 400, %2e%2e above the mount 404, %2e%2e inside it resolves (200)" \
    || fail "traversal (raw backslash '$st', %2e%2e out $e1 / $e2, inside $e3, control $ctl; want 400 404 404 200 1)"

# 24) A PROPFIND lists a directory larger than the 256 entries a fixed
#     stack array once held (and truncated to): davroot/many holds 300.
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/many/" 2>/dev/null || true)
n=$(grep -oE '<D:href>/dav/many/f[0-9]+\.txt</D:href>' <<< "$r" | sort -u | wc -l || true)
[[ "$n" == 300 ]] \
    && pass "PROPFIND of a 300-entry directory lists all 300" \
    || fail "large directory PROPFIND (listed $n; want 300)"

# 25) A mount whose root is the bare volume ("FS0:", 8082 /vol) serves the
#     volume's root: PROPFIND / lists what is there, GET reaches a file.
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE3}/vol/" 2>/dev/null || true)
g=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' "${BASE3}/vol/davroot/precious.txt" 2>/dev/null || echo 000)
efi=$(grep -ciF '<D:href>/vol/AxlTestNet.efi</D:href>' <<< "$r" || true)
[[ "$r" == *"<D:href>/vol/davroot/</D:href>"* && "$efi" == 1 && "$g" == 200 ]] \
    && pass "a mount at 'FS0:' lists the volume root and serves below it" \
    || fail "volume-root mount (davroot listed $([[ "$r" == *"/vol/davroot/<"* ]] && echo yes || echo no), AxlTestNet.efi $efi, GET $g; want yes 1 200)"

webdav_finish
