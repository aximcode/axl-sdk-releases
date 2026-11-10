#!/bin/bash
# test-meta: arch=x64 needs= est=75 local-only=0 guest=none
# guest=none: not converted yet (pool design §6.7 step 8 item 6): it reads
#   the boot disk's FAT image back with mtools.
# test-webdav-fs-put-qemu.sh -- axl_http_server_serve_fs end to end, part 2: PUT.
#
# The boot, the staging and the helpers are lib/webdav-fs-lib.sh, whose
# header says how the suite is split and why.
#
# Cases: PUT atomicity -- a hang-up, a digest, live temps hidden
#        and refused, stale temps, policy (13a-13e); one PUT per mount (14);
#        no-overwrite (15); a file only (16); room on the volume (17)
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# Usage: ./test/integration/test-webdav-fs-put-qemu.sh [--arch X64|AARCH64]

WEBDAV_PART="put"
WEBDAV_STALE_TEMP1=1
source "$(dirname "$0")/lib/webdav-fs-lib.sh"

# What 12 left: precious.txt, which 13c and 13c2 read as the old file.
put /dav/precious.txt "precious v2"

# 13) A PUT is ATOMIC: it writes a temp sibling and replaces the target
#     only once the whole body is stored, so a PUT that does not complete
#     -- the client hangs up, the Content-Digest does not match, the volume
#     fills -- leaves the old file exactly as it was. Before, the target was
#     truncated at the first chunk and an unfinished PUT left whatever had
#     arrived.

c=$(putc /dav/atom.txt "atom v1")
b=$(bodyz /dav/atom.txt)
[[ "$c" == 201 && "$b" == "atom v1" ]] \
    && pass "PUT atom.txt -> 201 'atom v1'" \
    || fail "setup PUT of atom.txt (got $c, body '$b'; want 201 'atom v1')"

# 13a) The client hangs up after 200,000 of 1 MiB -- three 64 KiB chunks
#      written -- and the old content survives exactly.
raw_put_hold "$HOST_PORT" /dav/atom.txt 1048576 200000
put_temps 1 || true
raw_put_drop
put_temps 0 || true
b=$(bodyz /dav/atom.txt)
[[ "$b" == "atom v1" ]] \
    && pass "a PUT whose client hung up mid-body leaves the old file exactly" \
    || fail "hung-up PUT over an existing file (body '$b'; want 'atom v1')"

# 13b) A Content-Digest that does not match the body is 400, decided BEFORE
#      the replace: the old content survives. RFC 9530's :base64: form and
#      the plain hex form are both read; every entry of the header is
#      scanned and the strongest algorithm present is checked; an
#      algorithm the SDK does not hash is ignored.
bad=0000000000000000000000000000000000000000000000000000000000000000
c=$(putc /dav/atom.txt "atom bad" -H "Content-Digest: sha-256=${bad}")
b=$(bodyz /dav/atom.txt)
[[ "$c" == 400 && "$b" == "atom v1" ]] \
    && pass "a hex Content-Digest that does not match -> 400, old file intact" \
    || fail "hex digest-mismatch PUT (got $c, body '$b'; want 400 'atom v1')"
c=$(putc /dav/atom.txt "atom bad" -H "Content-Digest: sha-256=:$(b64 sha256 other):")
b=$(bodyz /dav/atom.txt)
[[ "$c" == 400 && "$b" == "atom v1" ]] \
    && pass "an RFC 9530 Content-Digest that does not match -> 400, old file intact" \
    || fail "RFC digest-mismatch PUT (got $c, body '$b'; want 400 'atom v1')"
c=$(putc /dav/atom.txt "atom v2" -H "Content-Digest: sha-256=:$(b64 sha256 'atom v2'):")
b=$(bodyz /dav/atom.txt)
[[ "$c" == 201 && "$b" == "atom v2" ]] \
    && pass "an RFC 9530 Content-Digest that matches replaces the file -> 201 'atom v2'" \
    || fail "RFC digest-match PUT (got $c, body '$b'; want 201 'atom v2')"
c1=$(putc /dav/dg.txt "dg" -H "Content-Digest: md5=:AAAAAAAAAAAAAAAAAAAAAA==:, sha-256=:$(b64 sha256 other):")
g1=$(code GET /dav/dg.txt)
c2=$(putc /dav/dg.txt "dg" -H "Content-Digest: sha-256=:$(b64 sha256 dg):, sha-512=:$(b64 sha512 other):")
g2=$(code GET /dav/dg.txt)
c3=$(putc /dav/dg.txt "dg" -H "Content-Digest: md5=:AAAAAAAAAAAAAAAAAAAAAA==:")
c4=$(putc /dav/dg.txt "dg" -H "Content-Digest: md5=:AAAAAAAAAAAAAAAAAAAAAA==:, sha-256=:$(b64 sha256 dg):")
b=$(bodyz /dav/dg.txt)
[[ "$c1" == 400 && "$g1" == 404 && "$c2" == 400 && "$g2" == 404 && "$c3" == 201 && "$c4" == 201 && "$b" == "dg" ]] \
    && pass "Content-Digest: every entry scanned, the strongest checked, md5 ignored" \
    || fail "digest entries (md5+bad sha-256 $c1/$g1, good sha-256+bad sha-512 $c2/$g2, md5 only $c3, md5+good sha-256 $c4, body '$b'; want 400/404 400/404 201 201 'dg')"

# RFC 8941 lets a byte sequence drop its base64 padding; a supported
# algorithm whose entry carries no value is a mismatch, not a skip.
c1=$(putc /dav/dg.txt "dg2" -H "Content-Digest: sha-256=:$(b64 sha256 dg2 | tr -d '='):")
b1=$(bodyz /dav/dg.txt)
c2=$(putc /dav/dg.txt "dg3" -H "Content-Digest: md5=:AAAAAAAAAAAAAAAAAAAAAA==:, sha-256")
c3=$(putc /dav/dg.txt "dg3" -H "Content-Digest: sha-256=")
b3=$(bodyz /dav/dg.txt)
[[ "$c1" == 201 && "$b1" == "dg2" && "$c2" == 400 && "$c3" == 400 && "$b3" == "dg2" ]] \
    && pass "Content-Digest: unpadded base64 matches; a sha-256 entry with no value is 400" \
    || fail "digest forms (unpadded $c1 '$b1', valueless $c2, empty $c3, body '$b3'; want 201 'dg2' 400 400 'dg2')"

# 13c) While a PUT is in flight its temp is hidden and refused, by its exact
#      name and by any other spelling of it; the target still reads as the
#      old file. /fs-ls (the guest's own listing) names the temp -- and is
#      the control that one exists at all.
raw_put_hold "$HOST_PORT" /dav/atom.txt 1048576 200000
put_temps 1 || true
b=$(bodyz /dav/atom.txt)
tmpn=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' || true)
low=${tmpn,,}
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
listed=$(grep -ci -- "${tmpn:-NO-TEMP}" <<< "$r" || true)
cg=$(code GET "/dav/${tmpn}"); cgl=$(code GET "/dav/${low}")
cpf=$(code PROPFIND "/dav/${tmpn}" -H "Depth: 0")
cp=$(putc "/nodel/${tmpn}" "x"); cpl=$(putc "/nodel/${low}" "x")
cd=$(code DELETE "/dav/${low}")
cm=$(code MOVE "/dav/${tmpn}" -H "Destination: ${BASE}/dav/mvtemp.txt")
cc=$(code COPY /dav/precious.txt -H "Destination: ${BASE}/dav/${low}")
raw_put_drop
put_temps 0 || true
b2=$(bodyz /dav/atom.txt)
gone=$(fs_ls | grep -cxF -- "${tmpn:-NO-TEMP}" || true)
[[ "$tmpn" =~ $TEMP_RE && "$b" == "atom v2" && "$listed" == 0 ]] \
    && pass "during a PUT: the target reads as the old file, PROPFIND hides the temp ($tmpn)" \
    || fail "during a PUT (temp '$tmpn', target '$b', listed $listed; want ~APxxxxx.TMP 'atom v2' 0)"
[[ "$cg $cgl $cpf $cp $cpl $cd $cm $cc" == "404 404 404 403 403 404 404 409" ]] \
    && pass "a live temp is refused by name and by spelling: GET/PROPFIND/DELETE/MOVE 404, PUT 403, COPY onto it 409" \
    || fail "live temp requests (GET $cg/$cgl PROPFIND $cpf PUT $cp/$cpl DELETE $cd MOVE $cm COPY $cc; want 404 404 404 403 403 404 404 409)"
[[ "$b2" == "atom v2" && "$gone" == 0 ]] \
    && pass "the dropped PUT removed its temp, the target untouched" \
    || fail "after the dropped PUT (target '$b2', temp still listed $gone; want 'atom v2' 0)"

# 13c2) EDK2 FAT trims a leading blank, and trailing blanks and periods,
#       from a name before it looks it up -- so "~APxxxxx.TMP.",
#       "~APxxxxx.TMP%20" and "%20~APxxxxx.TMP" would reach the live temp
#       past its exact-name check. FAT cannot store such a name, so every
#       component spelled so is 400, on every method and in a Destination;
#       the temp is neither served, deleted, nor replaced.
raw_put_hold "$HOST_PORT" /dav/trim.txt 1048576 200000
put_temps 1 || true
tmpn=$(fs_ls | grep -E "$TEMP_RE" | grep -vxE '~AP0000[12]\.TMP' || true)
r=""
for sp in "${tmpn}." "${tmpn}%20" "%20${tmpn}" "${tmpn}.%20." ; do
    g=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' --path-as-is "${BASE}/dav/${sp}" 2>/dev/null || echo 000)
    p=$(putc "/nodel/${sp}" "x")
    d=$(code DELETE "/dav/${sp}")
    mv=$(code MOVE /dav/precious.txt -H "Destination: ${BASE}/dav/${sp}" -H "Overwrite: T")
    r+="$g$p$d$mv "
done
still=$(fs_ls | grep -cxF -- "${tmpn:-NO-TEMP}" || true)
raw_put_drop
put_temps 0 || true
bp=$(bodyz /dav/precious.txt)
g2=$(code GET "/dav/a%20/precious.txt"); g3=$(code GET "/dav/sub./x"); g4=$(code GET "/dav/%20sub/x")
[[ "$tmpn" =~ $TEMP_RE && "$r" == "400400400400 400400400400 400400400400 400400400400 " && "$still" == 1 \
   && "$bp" == "precious v2" && "$g2 $g3 $g4" == "400 400 400" ]] \
    && pass "a component with a leading blank or a trailing blank/period -> 400 on GET/PUT/DELETE/MOVE-onto; the live temp untouched" \
    || fail "trimmed spellings of a live temp (temp '$tmpn', GET/PUT/DELETE/MOVE per spelling '$r', temp still there $still, MOVE source '$bp', dir spellings $g2 $g3 $g4; want 400x16, 1, 'precious v2', 400 400 400)"

# 13d) A temp-shaped file that is NOT a live PUT's -- left by a power loss --
#      is an ordinary file: listed, readable, deletable.
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
b=$(bodyz /dav/~AP00001.TMP)
cd=$(code DELETE /dav/~AP00001.TMP)
cg=$(code GET /dav/~AP00001.TMP)
[[ "$r" == *"~AP00001.TMP"* && "$b" == "stale" && "$cd" == 204 && "$cg" == 404 ]] \
    && pass "a stale temp-shaped file is ordinary: listed, GET 'stale', DELETE 204" \
    || fail "stale temp (listed $([[ "$r" == *"~AP00001.TMP"* ]] && echo yes || echo no), body '$b', DELETE $cd, then GET $cg; want yes 'stale' 204 404)"

# 13e) A PUT the filesystem refuses by policy -- a read-only file -- is 403,
#      and the file is left as it was.
c=$(putc /dav/rofile.txt "clobber")
b=$(bodyz /dav/rofile.txt)
[[ "$c" == 403 && "$b" == "read only" ]] \
    && pass "PUT over a read-only file -> 403, file intact" \
    || fail "PUT over a read-only file (got $c, body '$b'; want 403 'read only')"

# 14) One PUT in flight per mount: while it holds the slot, another PUT --
#     with a body or without one -- is 503 with Retry-After: 1 and touches
#     nothing. An empty-body PUT used to bypass the slot and truncate.
raw_put_hold "$HOST_PORT" /dav/held.txt 1048576 200000
put_temps 1 || true
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X PUT -H "Content-Length: 0" \
    --data-binary "" "${BASE}/dav/atom.txt" 2>/dev/null || echo 000)
b=$(bodyz /dav/atom.txt)
[[ "$c" == 503 && "$b" == "atom v2" ]] \
    && pass "an empty-body PUT while another is in flight -> 503, target intact" \
    || fail "empty-body PUT during another (got $c, body '$b'; want 503 'atom v2')"
hdr=$(curl "${C[@]}" -D - -o /dev/null -T - "${BASE}/dav/other.txt" <<< "other" 2>/dev/null || true)
# The FINAL status line: a 100 Continue comes first (curl -T - asks for one).
st=$(printf '%s\n' "$hdr" | grep '^HTTP/' | tail -1 | tr -d '\r')
ra=$(printf '%s\n' "$hdr" | grep -i '^Retry-After:' | tr -d '\r' || true)
co=$(code GET /dav/other.txt)
[[ "$st" == "HTTP/1.1 503 Service Unavailable" && "$ra" == "Retry-After: 1" && "$co" == 404 ]] \
    && pass "a concurrent PUT -> 503 with Retry-After: 1, nothing created" \
    || fail "concurrent PUT (status '$st', '$ra', GET $co; want 503, 'Retry-After: 1', 404)"
raw_put_drop
put_temps 0 || true
ch=$(code GET /dav/held.txt)
c=$(putc /dav/other.txt "other")
b=$(bodyz /dav/other.txt)
[[ "$ch" == 404 && "$c" == 201 && "$b" == "other" ]] \
    && pass "the hung-up PUT left nothing, and freed the slot -> next PUT 201" \
    || fail "after the held PUT (GET held $ch, PUT $c, body '$b'; want 404 201 'other')"

# 15) A no-overwrite mount (a second server, 8081, over the same subtree)
#     refuses to replace an existing file -- 409, file intact -- and still
#     takes a new one. A file that appears at the target WHILE its PUT is in
#     flight is refused at the commit the same way.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE2}/dav/atom.txt" <<< "clobber" 2>/dev/null || echo 000)
b=$(bodyz /dav/atom.txt)
[[ "$c" == 409 && "$b" == "atom v2" ]] \
    && pass "no-overwrite PUT over an existing file -> 409, file intact" \
    || fail "no-overwrite PUT over an existing file (got $c, body '$b'; want 409 'atom v2')"
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE2}/dav/fresh.txt" <<< "fresh" 2>/dev/null || echo 000)
b=$(bodyz /dav/fresh.txt)
[[ "$c" == 201 && "$b" == "fresh" ]] \
    && pass "no-overwrite PUT of a new file -> 201" \
    || fail "no-overwrite PUT of a new file (got $c, body '$b'; want 201 'fresh')"
raw_put_hold "$HOST_PORT2" /dav/m1.txt 300000 200000
put_temps 1 || true
c1=$(putc /dav/m1.txt "m1 first")
st=$(raw_put_finish 100000)
b=$(bodyz /dav/m1.txt)
[[ "$c1" == 201 && "$st" == "HTTP/1.1 409 Conflict" && "$b" == "m1 first" ]] \
    && pass "no-overwrite: a file that appeared during the PUT -> 409 at the commit, file intact" \
    || fail "no-overwrite race (other PUT $c1, held '$st', body '$b'; want 201 'HTTP/1.1 409 Conflict' 'm1 first')"

# A MOVE or COPY onto an existing destination on the no-overwrite mount is
# 412 even with Overwrite: T, and both sides stay as they were.
c1=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X MOVE -H "Overwrite: T" \
    -H "Destination: ${BASE2}/dav/atom.txt" "${BASE2}/dav/fresh.txt" 2>/dev/null || echo 000)
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X COPY -H "Overwrite: T" \
    -H "Destination: ${BASE2}/dav/atom.txt" "${BASE2}/dav/fresh.txt" 2>/dev/null || echo 000)
ba=$(bodyz /dav/atom.txt)
bf=$(bodyz /dav/fresh.txt)
[[ "$c1" == 412 && "$c2" == 412 && "$ba" == "atom v2" && "$bf" == "fresh" ]] \
    && pass "no-overwrite: MOVE and COPY onto an existing file with Overwrite: T -> 412, both intact" \
    || fail "no-overwrite MOVE/COPY (got $c1 / $c2, target '$ba', source '$bf'; want 412 412 'atom v2' 'fresh')"

# 16) A PUT replaces a FILE only. A collection that appears at its target
#     while it is in flight -- made by MKCOL, or moved there -- is never
#     removed by the commit: 409, the collection and its contents intact.
raw_put_hold "$HOST_PORT" /dav/b1.txt 300000 200000
put_temps 1 || true
cm=$(code MKCOL /dav/b1.txt)
ci=$(putc /nodel/b1.txt/inner.txt "inner")
st=$(raw_put_finish 100000)
b=$(bodyz /dav/b1.txt/inner.txt)
[[ "$cm" == 201 && "$ci" == 201 && "$st" == "HTTP/1.1 409 Conflict" && "$b" == "inner" ]] \
    && pass "a collection MKCOL'd at the target during a PUT -> 409, its contents intact" \
    || fail "MKCOL during a PUT (MKCOL $cm, inner PUT $ci, held '$st', inner '$b'; want 201 201 'HTTP/1.1 409 Conflict' 'inner')"
raw_put_hold "$HOST_PORT" /dav/b2.txt 300000 200000
put_temps 1 || true
cm=$(code MKCOL /dav/b2dir)
ci=$(putc /nodel/b2dir/in.txt "in")
cv=$(code MOVE /dav/b2dir -H "Destination: ${BASE}/dav/b2.txt")
st=$(raw_put_finish 100000)
b=$(bodyz /dav/b2.txt/in.txt)
[[ "$cm $ci $cv" == "201 201 201" && "$st" == "HTTP/1.1 409 Conflict" && "$b" == "in" ]] \
    && pass "a collection MOVEd onto the target during a PUT -> 409, its contents intact" \
    || fail "MOVE during a PUT (MKCOL $cm, PUT $ci, MOVE $cv, held '$st', inner '$b'; want 201 201 201 'HTTP/1.1 409 Conflict' 'in')"

# 17) A PUT too big for the volume -- a 4 MiB body onto a 2 MB volume
#     (8081 /small). With a Content-Length it is 507 before a byte is
#     written; chunked, nothing states its size, so it runs out of room
#     mid-body and its chunk write fails: 500. Either way the old file is
#     exactly as it was, no temp is left, and the mount takes the next PUT.
head -c 4194304 /dev/zero > "$TEST_TMPDIR/big.bin"
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T "$TEST_TMPDIR/big.bin" "${BASE2}/small/old.txt" 2>/dev/null || true)
b=$(bodyz /small/old.txt "$BASE2")
l=$(fs_ls small | tr '\n' ' ')
[[ "$c" == 507 && "$b" == "old small" && "$l" == "old.txt " ]] \
    && pass "a PUT whose Content-Length the volume cannot hold -> 507, old file intact, no temp" \
    || fail "too-big PUT (got $c, body '$b', listing '$l'; want 507 'old small' 'old.txt ')"
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -H "Transfer-Encoding: chunked" \
    -T "$TEST_TMPDIR/big.bin" "${BASE2}/small/old.txt" 2>/dev/null || true)
b=$(bodyz /small/old.txt "$BASE2")
l=$(fs_ls small | tr '\n' ' ')
c2=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE2}/small/new.txt" <<< "fits" 2>/dev/null || echo 000)
[[ "$c" == 500 && "$b" == "old small" && "$l" == "old.txt " && "$c2" == 201 ]] \
    && pass "a chunked PUT that fills the volume -> 500, old file intact, no temp, next PUT 201" \
    || fail "volume-full chunked PUT (got $c, body '$b', listing '$l', next PUT $c2; want 500 'old small' 'old.txt ' 201)"

webdav_finish
