#!/bin/bash
# test-meta: arch=x64 needs= est=85 local-only=0 guest=none
# guest=none: not converted yet (pool design §6.7 step 8 item 6): it reads
#   the boot disk's FAT image back with mtools.
# test-webdav-inplace-qemu.sh — serve_fs PUT in place
# (AXL_SERVE_FS_PUT_IN_PLACE / _IF_SHORT), end to end.
#
# Boots AxlTestNet.efi serve-davfs, whose 8083 server holds the in-place
# mounts -- /ipat (atomic) and /ips (IF_SHORT) over a near-full RAM disk,
# /ipa (IN_PLACE) and /ipno (IN_PLACE + NO_OVERWRITE) over FS0:\davroot --
# and drives them with curl beside the 8080 server over the same
# directory. Split out of test-webdav-fs-qemu.sh, which it shares the
# fixture with, to keep each under CI's per-test limit.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# Usage: ./test/integration/test-webdav-inplace-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
HOST_PORT2=$(test_port 1)   # the no-overwrite server (guest 8081)
HOST_PORT3=$(test_port 2)   # the case-sensitive volume (guest 8082)
HOST_PORT4=$(test_port 3)   # the PUT-in-place mounts (guest 8083)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
test_add_efi "$(test_build_dir "$_native_arch")/AxlTestNet.efi"

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
echo Starting WebDAV file server...
AxlTestNet.efi serve-davfs
NSHEOF

# A read-only file a PUT in place may not replace (403); a collection with a
# long name, reached through its 8.3 alias; and a file shaped like a PUT's
# temp that no PUT owns -- the control for the final on-disk check.
mkdir -p "$TEST_STAGING/davroot/longdirectoryname"
printf 'read only\n' > "$TEST_STAGING/davroot/rofile.txt"
printf 'stale\n' > "$TEST_STAGING/davroot/~AP00002.TMP"

test_build_image
if ! mattrib -i "$TEST_DISK" +r ::/davroot/rofile.txt; then
    echo "FAIL: cannot set the read-only attribute on the staged files"
    exit 1
fi

echo "=== AxlNet serve_fs PUT in place Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT" "$HOST_PORT2" 8081 "$HOST_PORT3" 8082 \
    "$HOST_PORT4" 8083
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

BASE="http://127.0.0.1:${HOST_PORT}"
C=(-s -H "Connection: close" --max-time 10)
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# code <method> <path> [extra curl args...]
code() {
    local m="$1" path="$2"; shift 2
    status_of "$m $path" curl "${C[@]}" -o /dev/null -w '%{http_code}' -X "$m" "$@" "${BASE}${path}"
}
# F11: one run in ~30 once saw a request get no HTTP status at all (curl
# 000) and it did not recur in 2,100 looped requests, idle or under full
# CPU load. So a recurrence must explain itself: status_of runs a curl
# that prints a status, and when there is none it prints, to stderr,
# curl's EXIT CODE (7 refused, 28 timed out, 52 empty reply, 56 reset --
# each a different fault), the host's TCP state for the server port, and
# the guest log's tail (a rejected client is "no free connection slots",
# at debug level). It still answers 000 on stdout, so callers are unchanged.
# status_of <what> <curl command...>
status_of() {
    local what="$1" out rc
    shift
    out=$("$@" 2>/dev/null) && rc=0 || rc=$?
    if [[ "$rc" == 0 && "$out" =~ ^[1-5][0-9][0-9]$ ]]; then
        echo "$out"
        return
    fi
    {
        echo "  DIAG: '$what' got no HTTP status: curl exit $rc, wrote '$out', at $(date +%T.%N)"
        ss -tan "( sport = :${HOST_PORT} or dport = :${HOST_PORT} )" 2>/dev/null \
            | sed 's/^/  DIAG tcp: /' | head -20
        test_clean_log
        tail -30 "$TEST_CLEAN_LOG" | sed 's/^/  DIAG guest: /'
    } >&2
    echo 000
}
# The capture's control: a request to a port nothing listens on must come
# back 000 WITH its diagnosis (curl exit 7), or the capture cannot see.
s0=$(status_of "control" curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
     "http://127.0.0.1:1/" 2>"$TEST_TMPDIR/diag-control.txt")
[[ "$s0" == 000 ]] && grep -q "^  DIAG: 'control' got no HTTP status: curl exit 7," "$TEST_TMPDIR/diag-control.txt" \
    && pass "the no-status capture sees a refused connection (000, curl exit 7)" \
    || fail "no-status capture control (got '$s0', diag '$(head -1 "$TEST_TMPDIR/diag-control.txt")')"
# body <path>
body() { curl "${C[@]}" "${BASE}$1" 2>/dev/null || true; }
in2xx() { [[ "$1" == 2* ]]; }

put() { curl "${C[@]}" -o /dev/null -T - "${BASE}$1" <<< "$2" 2>/dev/null || true; }
# putc <path> <body> [curl args...]: PUT <body> plus a newline; the status.
putc() {
    local p="$1" d="$2"
    shift 2
    status_of "PUT $p" curl "${C[@]}" -o /dev/null -w '%{http_code}' "$@" -T - "${BASE}${p}" <<< "$d"
}

# raw_put_hold <port> <path> <content-length> <bytes>: send a PUT's headers
# and <bytes> of its body on fd 4, and keep the connection open.
# The connection is fd 4, or fd $RAW_FD for a second PUT held at once.
raw_put_hold() {
    local fd=${RAW_FD:-4}
    eval "exec ${fd}<>/dev/tcp/127.0.0.1/$1"
    printf 'PUT %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %s\r\n\r\n' "$2" "$3" >&"$fd"
    head -c "$4" /dev/zero >&"$fd"
}
# raw_put_finish <bytes>: send the rest of the held body; print the status line.
raw_put_finish() {
    local fd=${RAW_FD:-4}
    head -c "$1" /dev/zero >&"$fd"
    timeout 10 head -1 <&"$fd" | tr -d '\r' || true
    eval "exec ${fd}>&-"
}
raw_put_drop() { local fd=${RAW_FD:-4}; eval "exec ${fd}>&-"; }
# bodyz <path> [base]: the body with any NUL bytes dropped (a zero-filled
# partial file reads as '' rather than tripping the shell's NUL warning).
bodyz() { curl "${C[@]}" "${2:-$BASE}$1" 2>/dev/null | tr -d '\0' || true; }
# fs_ls [small | d=<dir>]: the served directory (or one below it) as the
# guest's filesystem lists it -- the only listing that shows a live temp.
fs_ls() { curl "${C[@]}" "${BASE}/fs-ls${1:+?$1}" 2>/dev/null | tr -d '\r' || true; }
TEMP_RE='^~AP[0-9A-F]{5}\.TMP$'

# 22) PUT in place (8083). /ipat is atomic (the default) and /ips
#     IN_PLACE_IF_SHORT, both over AXLIPRD: big.bin (1,200,000 'A') on a
#     volume with less than 1,100,000 bytes free, so a second copy of a
#     body near its size does not fit. /ipa (IN_PLACE) and /ipno (IN_PLACE
#     + NO_OVERWRITE) serve FS0:\davroot.
BASE4="http://127.0.0.1:${HOST_PORT4}"
# sha <file-or-> : the sha256 of stdin or a file, hex only.
sha() { sha256sum "$@" | cut -d' ' -f1; }
# getsha <url>: the sha256 of what GET returns.
getsha() { curl "${C[@]}" "$1" 2>/dev/null | sha; }
SHA_BIG_A=$(head -c 1200000 /dev/zero | tr '\0' 'A' | sha)
SHA_Z11=$(head -c 1100000 /dev/zero | sha)
head -c 1100000 /dev/zero > "$TEST_TMPDIR/z11.bin"

# 22a) Atomic, a Content-Length the volume cannot hold beside the file it
#      replaces: 507, nothing touched. And IF_SHORT with the same body sent
#      CHUNKED: no length to decide by, so atomic -- it fills the volume,
#      500, and big.bin is untouched.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T "$TEST_TMPDIR/z11.bin" "${BASE4}/ipat/big.bin" 2>/dev/null || echo 000)
h=$(getsha "${BASE4}/ipat/big.bin")
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -H "Transfer-Encoding: chunked" \
    -T "$TEST_TMPDIR/z11.bin" "${BASE4}/ips/big.bin" 2>/dev/null || echo 000)
h2=$(getsha "${BASE4}/ipat/big.bin")
l=$(fs_ls ip | tr '\n' ' ')
[[ "$c" == 507 && "$h" == "$SHA_BIG_A" && "$c2" == 500 && "$h2" == "$SHA_BIG_A" && "$l" == "small.txt big.bin " ]] \
    && pass "near-full volume: atomic PUT -> 507; IF_SHORT chunked stays atomic -> 500; big.bin intact, no temp" \
    || fail "near-full atomic (atomic $c, chunked IF_SHORT $c2, big.bin $([[ "$h" == "$SHA_BIG_A" && "$h2" == "$SHA_BIG_A" ]] && echo intact || echo CHANGED), listing '$l'; want 507 500 intact 'small.txt big.bin ')"

# 22b) IF_SHORT, the same body with its Content-Length: in place. Held
#      part-way, the volume shows no temp (it went in place), then 201 and
#      big.bin is the new body; the decision is logged.
raw_put_hold "$HOST_PORT4" /ips/big.bin 1100000 200000
sleep 2
l=$(fs_ls ip | tr '\n' ' ')
st=$(raw_put_finish 900000)
h=$(getsha "${BASE4}/ipat/big.bin")
test_clean_log
lg=$(grep -cE "PUT in place: 'AXLIPRD:.big\.bin' \(1100000 bytes\): the volume has [0-9]+ bytes free, too few for a second copy" "$TEST_CLEAN_LOG" || true)
[[ "$l" == "small.txt big.bin " && "$st" == "HTTP/1.1 201 Created" && "$h" == "$SHA_Z11" && "$lg" == 1 ]] \
    && pass "IF_SHORT on a near-full volume: in place (no temp seen), 201, the new body, logged" \
    || fail "IF_SHORT in place (listing mid-PUT '$l', status '$st', big.bin $([[ "$h" == "$SHA_Z11" ]] && echo new || echo WRONG), log lines $lg; want 'small.txt big.bin ' 201 new 1)"

# 22b2) IF_SHORT, a body the volume cannot hold even in place (3,000,000
#       bytes on a 2 MB volume): 507 before anything is touched -- big.bin
#       is not truncated for a write that could never finish.
raw_put_hold "$HOST_PORT4" /ips/big.bin 3000000 70000
st=$(raw_put_finish 0)
h=$(getsha "${BASE4}/ipat/big.bin")
[[ "$st" == "HTTP/1.1 507 Insufficient Storage" && "$h" == "$SHA_Z11" ]] \
    && pass "IF_SHORT, too big even in place -> 507, big.bin untouched" \
    || fail "IF_SHORT too big (status '$st', big.bin $([[ "$h" == "$SHA_Z11" ]] && echo intact || echo CHANGED); want 507 intact)"

# 22c) IF_SHORT with room: atomic. Held part-way, the volume shows the
#      temp; dropped, small.txt is exactly as it was. While that temp is
#      live, an IN_PLACE PUT naming it is refused (403) like any other.
raw_put_hold "$HOST_PORT4" /ips/small.txt 300000 200000
sleep 2
tmpn=$(fs_ls ip | grep -E "$TEMP_RE" || true)
raw_put_drop
sleep 2
b=$(bodyz /ipat/small.txt "$BASE4")
raw_put_hold "$HOST_PORT" /dav/lt.txt 300000 200000
sleep 2
ltmp=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' || true)
cl=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE4}/ipa/${ltmp:-NO-TEMP}" <<< "x" 2>/dev/null || echo 000)
raw_put_drop
sleep 2
[[ "$tmpn" =~ $TEMP_RE && "$b" == "small v1" && "$ltmp" =~ $TEMP_RE && "$cl" == 403 ]] \
    && pass "IF_SHORT with room: atomic (temp $tmpn seen), dropped -> intact; IN_PLACE onto a live temp -> 403" \
    || fail "IF_SHORT with room (temp '$tmpn', small.txt '$b', live temp '$ltmp', IN_PLACE PUT onto it $cl; want ~APxxxxx.TMP 'small v1' ~APxxxxx.TMP 403)"

# 22c2) The margin: a body of EXACTLY the volume's free space would fit as
#       a temp only if the directory never grew, so IF_SHORT counts one
#       allocation unit more and goes in place: no temp, and logged.
read -r fr un <<< "$(curl "${C[@]}" "${BASE}/fs-free" 2>/dev/null || true)"
raw_put_hold "$HOST_PORT4" /ips/small.txt "${fr:-0}" 70000
sleep 2
l=$(fs_ls ip | grep -E "$TEMP_RE" | tr '\n' ' ' || true)
raw_put_drop
sleep 2
test_clean_log
lg=$(grep -cE "PUT in place: 'AXLIPRD:.small\\.txt' \\(${fr:-0} bytes\\): the volume has ${fr:-0} bytes free" "$TEST_CLEAN_LOG" || true)
[[ "${fr:-0}" -gt 70000 && $(( ${fr:-0} % ${un:-1} )) == 0 && -z "$l" && "$lg" == 1 ]] \
    && pass "IF_SHORT, a body of exactly the free space ($fr) -> in place (the one-unit margin), logged" \
    || fail "margin (free '$fr' unit '$un', temps '$l', log lines $lg; want no temp, 1 line)"

# 22c3) The margin is the directory room the commit's own replace will
#       need, counted as axl_file_replace counts it: a 204-character long
#       name takes 17 entries for the new name and 17 for the undo's, plus
#       the aside's and the temp's -- three 512-byte units, not one. A body
#       two units short of the free space therefore goes in place (the
#       atomic path would upload it all and then be refused 507 at the
#       commit's own pre-flight).
LN="$(printf 'L%.0s' $(seq 1 200)).txt"
c0=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE4}/ipat/${LN}" <<< "long" 2>/dev/null || echo 000)
read -r fr un <<< "$(curl "${C[@]}" "${BASE}/fs-free" 2>/dev/null || true)"
bl=$(( ${fr:-0} - 2 * ${un:-512} ))
raw_put_hold "$HOST_PORT4" "/ips/${LN}" "$bl" 70000
sleep 2
l=$(fs_ls ip | grep -E "$TEMP_RE" | tr '\n' ' ' || true)
raw_put_drop
sleep 2
test_clean_log
lg=$(grep -cE "PUT in place: 'AXLIPRD:.L{200}\\.txt' \\(${bl} bytes\\)" "$TEST_CLEAN_LOG" || true)
[[ "$c0" == 201 && "$bl" -gt 70000 && -z "$l" && "$lg" == 1 ]] \
    && pass "IF_SHORT counts a long name's directory room: a body two units short of the free space goes in place" \
    || fail "long-name margin (create $c0, body $bl, temps '$l', log lines $lg; want 201, no temp, 1 line)"

# 22d) IN_PLACE, the client hangs up mid-body: no temp, the file is
#      PARTIALLY written -- what arrived, the old content gone -- and the
#      server logged an error naming it with the byte count.
put /dav/ipart.txt "ipart old"
raw_put_hold "$HOST_PORT4" /ipa/ipart.txt 600000 200000
sleep 2
l=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' | tr '\n' ' ' || true)
raw_put_drop
sleep 2
sz=$(curl "${C[@]}" -o /dev/null -w '%{size_download}' "${BASE}/dav/ipart.txt" 2>/dev/null || echo 0)
nz=$(curl "${C[@]}" "${BASE}/dav/ipart.txt" 2>/dev/null | tr -d '\0' | wc -c || true)
test_clean_log
lg=$(grep -oE "PUT in place: 'FS0:.davroot.ipart\.txt' is partially written \([0-9]+ of 600000 bytes\); its old content is gone" "$TEST_CLEAN_LOG" || true)
ln=$(grep -oE '\([0-9]+ of' <<< "$lg" | tr -dc '0-9' || true)
[[ -z "$l" && "$sz" -gt 0 && "$sz" -lt 600000 && "$nz" == 0 && "$ln" == "$sz" ]] \
    && pass "IN_PLACE hang-up: no temp, partial file ($sz zero bytes), error names it with that count" \
    || fail "IN_PLACE hang-up (temps '$l', size $sz, non-zero bytes $nz, logged '$lg'; want '' 0<size<600000 0 the same count)"

# 22e) IN_PLACE, a Content-Digest that does not match: 400, and the file
#      now HOLDS that body -- the response says so, and so does the log.
put /dav/idg.txt "idg old"
bad=0000000000000000000000000000000000000000000000000000000000000000
r=$(curl "${C[@]}" -w '\n%{http_code}' -H "Content-Digest: sha-256=${bad}" -T - "${BASE4}/ipa/idg.txt" <<< "idg bad" 2>/dev/null || true)
c=$(tail -1 <<< "$r"); rb=$(sed '$d' <<< "$r")
b=$(bodyz /dav/idg.txt)
test_clean_log
lg=$(grep -cE "PUT Content-Digest mismatch \(client claimed ${bad}, body hashed to [0-9a-f]{64}\); the resource was written in place and now holds that body" "$TEST_CLEAN_LOG" || true)
[[ "$c" == 400 && "$rb" == "Content-Digest mismatch: the resource now holds the body that did not match" \
   && "$b" == "idg bad" && "$lg" == 1 ]] \
    && pass "IN_PLACE digest mismatch -> 400 saying so, the file holds the bad body, logged" \
    || fail "IN_PLACE digest mismatch (got $c, body '$rb', file '$b', log lines $lg; want 400 the message 'idg bad' 1)"

# 22e2) IN_PLACE goes in place only over an EXISTING file: a new one takes
#       the temp like any atomic PUT -- held part-way, the temp is on the
#       volume and the file is not; dropped, nothing is left.
raw_put_hold "$HOST_PORT4" /ipa/ipnew.txt 300000 200000
sleep 2
tmpn=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' || true)
there=$(fs_ls | grep -cxF "ipnew.txt" || true)
raw_put_drop
sleep 2
left=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' | tr '\n' ' ' || true)
cg=$(code GET /dav/ipnew.txt)
[[ "$tmpn" =~ $TEMP_RE && "$there" == 0 && -z "$left" && "$cg" == 404 ]] \
    && pass "IN_PLACE, a NEW file: through the temp ($tmpn), and a dropped PUT leaves nothing" \
    || fail "IN_PLACE new file (temp '$tmpn', target listed $there, left '$left', GET $cg; want ~APxxxxx.TMP 0 '' 404)"

# 22e3) A file being written IN PLACE is busy for every mount, not only
#       the one writing it (8083 /ipa and 8080 /dav serve one directory):
#       another PUT -- by any letter case -- is 503 with Retry-After: 1;
#       a DELETE, a MOVE of it or onto it, and a COPY onto it are 423
#       Locked; all change nothing. Once the PUT ends, it is free again.
put /dav/busy.txt "busy old"
put /dav/busy2.txt "busy two"
raw_put_hold "$HOST_PORT4" /ipa/busy.txt 600000 200000
sleep 2
hdr=$(curl "${C[@]}" -D - -o /dev/null -T - "${BASE}/dav/busy.txt" <<< "clobber" 2>/dev/null || true)
# The FINAL status line: a 100 Continue comes first (curl -T - asks for one).
st=$(printf '%s\n' "$hdr" | grep '^HTTP/' | tail -1 | tr -d '\r')
ra=$(printf '%s\n' "$hdr" | grep -i '^Retry-After:' | tr -d '\r' || true)
cu=$(putc /dav/BUSY.TXT "clobber")
cd=$(code DELETE /dav/busy.txt)
cm=$(code MOVE /dav/busy.txt -H "Destination: ${BASE}/dav/busy3.txt")
cn=$(code MOVE /dav/busy2.txt -H "Destination: ${BASE}/dav/busy.txt" -H "Overwrite: T")
cc=$(code COPY /dav/busy2.txt -H "Destination: ${BASE}/dav/busy.txt" -H "Overwrite: T")
b2=$(bodyz /dav/busy2.txt)
c3=$(code GET /dav/busy3.txt)
raw_put_drop
sleep 2
ca=$(putc /dav/busy.txt "after")
ba=$(bodyz /dav/busy.txt)
[[ "$st" == "HTTP/1.1 503 Service Unavailable" && "$ra" == "Retry-After: 1" && "$cu" == 503 \
   && "$cd $cm $cn $cc" == "423 423 423 423" && "$b2" == "busy two" && "$c3" == 404 \
   && "$ca" == 201 && "$ba" == "after" ]] \
    && pass "a file written in place is busy on every mount: PUT 503 (any case), DELETE/MOVE/MOVE-onto/COPY-onto 423; free after" \
    || fail "busy in-place target (PUT '$st' '$ra', case PUT $cu, DELETE $cd MOVE $cm MOVE-onto $cn COPY-onto $cc, busy2 '$b2', busy3 $c3, after $ca '$ba'; want 503 'Retry-After: 1' 503, 423 x4, 'busy two' 404, 201 'after')"

# 22e4) The busy file is known by the names the filesystem STORES, as
#       axl_file_replace knows an entry -- not by how a request spells it.
#       With a long-named file busy inside a long-named collection, a PUT
#       through the file's 8.3 alias is 503, a DELETE of the collection
#       through ITS alias 423, and a MOVE onto the file through its alias
#       423; nothing changes.
put /dav/longdirectoryname/busylongtarget.txt "long old"
put /dav/busysrc.txt "src"
raw_put_hold "$HOST_PORT4" /ipa/longdirectoryname/busylongtarget.txt 600000 200000
sleep 2
ka=$(putc /dav/longdirectoryname/BUSYLO~1.TXT "clobber")
kb=$(putc /dav/LONGDI~1/busylongtarget.txt "clobber")
kd=$(code DELETE /dav/LONGDI~1)
km=$(code MOVE /dav/busysrc.txt -H "Destination: ${BASE}/dav/LONGDI~1/BUSYLO~1.TXT" -H "Overwrite: T")
bs=$(bodyz /dav/busysrc.txt)
raw_put_drop
sleep 2
lsd=$(fs_ls d=longdirectoryname | grep -cxF "busylongtarget.txt" || true)
[[ "$ka $kb $kd $km" == "503 503 423 423" && "$bs" == "src" && "$lsd" == 1 ]] \
    && pass "busy by stored name: PUT via the file's alias 503, via the parent's alias 503, DELETE of the parent's alias 423, MOVE onto the alias 423" \
    || fail "busy through aliases (PUT file alias $ka, PUT parent alias $kb, DELETE parent alias $kd, MOVE onto alias $km, source '$bs', file listed $lsd; want 503 503 423 423 'src' 1)"

# 22f) The refusals still come first, the file untouched: an existing file
#      on IN_PLACE + NO_OVERWRITE -> 409; a read-only file -> 403; and a PUT
#      through an existing file's 8.3 alias writes it under its long name.
c1=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE4}/ipno/idg.txt" <<< "clobber" 2>/dev/null || echo 000)
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE4}/ipno/inofresh.txt" <<< "fresh" 2>/dev/null || echo 000)
c3=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE4}/ipa/rofile.txt" <<< "clobber" 2>/dev/null || echo 000)
put /dav/inplacealiastarget.txt "alias v1"
c4=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE4}/ipa/INPLAC~1.TXT" <<< "alias v2" 2>/dev/null || echo 000)
b1=$(bodyz /dav/idg.txt); b2=$(bodyz /dav/inofresh.txt); b3=$(bodyz /dav/rofile.txt); b4=$(bodyz /dav/inplacealiastarget.txt)
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
[[ "$c1 $c2 $c3 $c4" == "409 201 403 201" && "$b1" == "idg bad" && "$b2" == "fresh" && "$b3" == "read only" \
   && "$b4" == "alias v2" && "$r" == *"/dav/inplacealiastarget.txt<"* && "$r" != *"/dav/INPLAC~1.TXT<"* ]] \
    && pass "IN_PLACE refusals first: no-overwrite 409 (a new file 201), read-only 403; through an alias -> 201 under the long name" \
    || fail "IN_PLACE refusals (no-overwrite $c1 '$b1', new $c2 '$b2', read-only $c3 '$b3', alias $c4 '$b4'; want 409 'idg bad' 201 'fresh' 403 'read only' 201 'alias v2', long name listed)"


# No PUT left a temp on the volume: read the disk itself once the guest is
# down. The staged ~AP00002.TMP is the control -- the listing must see it.
kill "$TEST_QEMU_PID" 2>/dev/null || true
wait "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0
left=$(mdir -i "$TEST_DISK" -/ -b ::/davroot 2>/dev/null | grep -E '/~AP[0-9A-F]{5}\.TMP$' || true)
[[ "$left" == "::/davroot/~AP00002.TMP" ]] \
    && pass "no PUT temp left on the volume (only the staged stale one)" \
    || fail "PUT temps on the volume: '$(tr '\n' ' ' <<< "$left")' (want only ::/davroot/~AP00002.TMP)"

echo ""
echo "  serve_fs PUT in place: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ "$FAIL" -eq 0 ]] || exit 1
exit 0
