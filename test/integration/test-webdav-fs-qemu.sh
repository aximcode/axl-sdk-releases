#!/bin/bash
# test-meta: arch=x64 needs= est=63 local-only=0 guest=none
# guest=none: not converted yet (pool design §6.7 step 8 item 6): it reads
#   the boot disk's FAT image back with mtools.
# test-webdav-fs-qemu.sh -- axl_http_server_serve_fs end to end, part 1: the verbs.
#
# The boot, the staging and the helpers are lib/webdav-fs-lib.sh, whose
# header says how the suite is split and why.
#
# Cases: PUT/GET, PROPFIND, MKCOL, MOVE, COPY, DELETE (1-6),
#        overwrite onto collections and asides (6b, 6c), traversal (7), the
#        read-only mount (8), keep-alive framing (8b), auth (9-11), a failed
#        PUT that must not wedge the mount (12)
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# Usage: ./test/integration/test-webdav-fs-qemu.sh [--arch X64|AARCH64]

WEBDAV_PART="verbs"
source "$(dirname "$0")/lib/webdav-fs-lib.sh"

# 1) PUT a file (streaming write), then GET it back.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE}/dav/hello.txt" <<< "Hello WebDAV" 2>/dev/null || echo 000)
in2xx "$c" && pass "PUT /dav/hello.txt -> $c" || fail "PUT /dav/hello.txt (got $c)"

c=$(code GET /dav/hello.txt)
b=$(body /dav/hello.txt)
[[ "$c" == 200 && "$b" == *"Hello WebDAV"* ]] && pass "GET returns the written body" \
    || fail "GET /dav/hello.txt (code $c, body '$b')"

# 2) PROPFIND the collection lists the file.
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
echo "$r" | grep -q "hello.txt" && pass "PROPFIND /dav/ lists hello.txt" \
    || fail "PROPFIND /dav/ missing hello.txt"

# 3) MKCOL a subdirectory.
c=$(code MKCOL /dav/sub)
in2xx "$c" && pass "MKCOL /dav/sub -> $c" || fail "MKCOL /dav/sub (got $c)"

# 4) MOVE the file into the subdir.
c=$(code MOVE /dav/hello.txt -H "Destination: ${BASE}/dav/sub/moved.txt")
in2xx "$c" && pass "MOVE hello.txt -> sub/moved.txt ($c)" || fail "MOVE (got $c)"
c=$(code GET /dav/sub/moved.txt); [[ "$c" == 200 ]] && pass "moved file readable at new path" || fail "GET moved (got $c)"
c=$(code GET /dav/hello.txt);     [[ "$c" == 404 ]] && pass "old path is now 404" || fail "old path (got $c)"

# 5) COPY the file, original stays.
c=$(code COPY /dav/sub/moved.txt -H "Destination: ${BASE}/dav/copy.txt")
in2xx "$c" && pass "COPY -> /dav/copy.txt ($c)" || fail "COPY (got $c)"
c=$(code GET /dav/copy.txt);        [[ "$c" == 200 ]] && pass "copy readable" || fail "GET copy (got $c)"
c=$(code GET /dav/sub/moved.txt);   [[ "$c" == 200 ]] && pass "COPY left the source in place" || fail "source gone (got $c)"

# 6) DELETE the copy.
c=$(code DELETE /dav/copy.txt)
in2xx "$c" && pass "DELETE /dav/copy.txt ($c)" || fail "DELETE (got $c)"
c=$(code GET /dav/copy.txt); [[ "$c" == 404 ]] && pass "deleted file is 404" || fail "deleted GET (got $c)"

# 6b) MOVE with Overwrite onto an existing COLLECTION. RFC 4918 §9.9.3: with
#     Overwrite: T the server deletes the destination (Depth: infinity) and
#     moves -- 204, as the destination existed; with Overwrite: F it is 412
#     and nothing changes. A collection, empty or not, is no exception.
curl "${C[@]}" -o /dev/null -T - "${BASE}/dav/mv1.txt" <<< "one" 2>/dev/null || true
code MKCOL /dav/emptycol >/dev/null
c=$(code MOVE /dav/mv1.txt -H "Destination: ${BASE}/dav/emptycol" -H "Overwrite: T")
b=$(body /dav/emptycol)
[[ "$c" == 204 && "$b" == "one" ]] && pass "MOVE file onto an empty collection, Overwrite T -> 204, file there" \
    || fail "MOVE onto an empty collection (got $c, body '$b', want 204 'one')"

curl "${C[@]}" -o /dev/null -T - "${BASE}/dav/mv2.txt" <<< "two" 2>/dev/null || true
code MKCOL /dav/fullcol >/dev/null
curl "${C[@]}" -o /dev/null -T - "${BASE}/dav/fullcol/inner.txt" <<< "inner" 2>/dev/null || true
c=$(code MOVE /dav/mv2.txt -H "Destination: ${BASE}/dav/fullcol" -H "Overwrite: T")
b=$(body /dav/fullcol)
ci=$(code GET /dav/fullcol/inner.txt)
[[ "$c" == 204 && "$b" == "two" && "$ci" == 404 ]] \
    && pass "MOVE file onto a non-empty collection, Overwrite T -> 204, old tree gone" \
    || fail "MOVE onto a non-empty collection (got $c, body '$b', inner $ci; want 204 'two' 404)"

curl "${C[@]}" -o /dev/null -T - "${BASE}/dav/mv3.txt" <<< "three" 2>/dev/null || true
code MKCOL /dav/keepcol >/dev/null
c=$(code MOVE /dav/mv3.txt -H "Destination: ${BASE}/dav/keepcol" -H "Overwrite: F")
b=$(body /dav/mv3.txt)
ck=$(code PROPFIND /dav/keepcol -H "Depth: 0")
[[ "$c" == 412 && "$b" == "three" && "$ck" == 207 ]] \
    && pass "MOVE onto an existing collection, Overwrite F -> 412, nothing changed" \
    || fail "MOVE Overwrite F (got $c, source '$b', collection $ck; want 412 'three' 207)"

# A destination that HOLDS the source is not deleted to make room: that
# would delete what is being moved.
code MKCOL /dav/parentcol >/dev/null
curl "${C[@]}" -o /dev/null -T - "${BASE}/dav/parentcol/child.txt" <<< "child" 2>/dev/null || true
c=$(code MOVE /dav/parentcol/child.txt -H "Destination: ${BASE}/dav/parentcol" -H "Overwrite: T")
b=$(body /dav/parentcol/child.txt)
[[ "$c" == 403 && "$b" == "child" ]] \
    && pass "MOVE onto the collection holding the source -> 403, source kept" \
    || fail "MOVE onto its own parent (got $c, source '$b'; want 403 'child')"

# 6c) An overwrite never removes the destination before the move has
#     happened: the destination is renamed ASIDE, the move made, and only
#     then is the aside removed -- so a move that cannot happen costs nothing.

# A source that does not exist is 404, and the destination stays.
code MKCOL /dav/victim >/dev/null
put /dav/victim/v.txt "victim"
c=$(code MOVE /dav/nosuch.txt -H "Destination: ${BASE}/dav/victim" -H "Overwrite: T")
b=$(body /dav/victim/v.txt)
[[ "$c" == 404 && "$b" == "victim" ]] \
    && pass "MOVE of a missing source -> 404, destination tree intact" \
    || fail "MOVE of a missing source (got $c, victim '$b'; want 404 'victim')"

# A destination spelled through its 8.3 alias that HOLDS the source: no
# string compare can see it, but the names the filesystem stores do, so the
# move is refused (403) before anything is set aside.
code MKCOL /dav/longdirectoryname >/dev/null
put /dav/longdirectoryname/child.txt "child"
put /dav/longdirectoryname/sibling.txt "sibling"
c=$(code MOVE /dav/longdirectoryname/child.txt -H "Destination: ${BASE}/dav/LONGDI~1" -H "Overwrite: T")
b1=$(body /dav/longdirectoryname/child.txt)
b2=$(body /dav/longdirectoryname/sibling.txt)
[[ "$c" == 403 && "$b1" == "child" && "$b2" == "sibling" ]] \
    && pass "MOVE onto the 8.3 alias of the source's parent -> 403, everything intact" \
    || fail "MOVE onto the parent's 8.3 alias (got $c, child '$b1', sibling '$b2'; want 403 'child' 'sibling')"
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
[[ "$r" == *"/dav/longdirectoryname/"* ]] && ! grep -qE '~AXL[0-9A-F]{4}\.TMP' <<< "$r" \
    && pass "the refused MOVE left the parent under its own name, no aside" \
    || fail "after the refused alias MOVE the parent is not back under its name"

# A collection onto an existing FILE, Overwrite T: 204 (RFC 4918 §9.9.3).
code MKCOL /dav/srccol >/dev/null
put /dav/srccol/a.txt "inner-a"
put /dav/destfile.txt "plain file"
c=$(code MOVE /dav/srccol -H "Destination: ${BASE}/dav/destfile.txt" -H "Overwrite: T")
b=$(body /dav/destfile.txt/a.txt)
cs=$(code GET /dav/srccol/a.txt)
[[ "$c" == 204 && "$b" == "inner-a" && "$cs" == 404 ]] \
    && pass "MOVE a collection onto a file, Overwrite T -> 204, the collection is there" \
    || fail "MOVE collection onto a file (got $c, inner '$b', old $cs; want 204 'inner-a' 404)"

# COPY onto an existing collection, Overwrite T: 204, old tree gone
# (RFC 4918 §9.8.4); Overwrite F: 412, nothing changed (§9.8.5).
code MKCOL /dav/cpdst >/dev/null
put /dav/cpdst/old.txt "old"
put /dav/cpsrc.txt "copied"
c=$(code COPY /dav/cpsrc.txt -H "Destination: ${BASE}/dav/cpdst" -H "Overwrite: F")
b=$(body /dav/cpdst/old.txt)
[[ "$c" == 412 && "$b" == "old" ]] \
    && pass "COPY onto an existing collection, Overwrite F -> 412, nothing changed" \
    || fail "COPY Overwrite F (got $c, old '$b'; want 412 'old')"
c=$(code COPY /dav/cpsrc.txt -H "Destination: ${BASE}/dav/cpdst" -H "Overwrite: T")
b=$(body /dav/cpdst)
co=$(code GET /dav/cpdst/old.txt)
bs=$(body /dav/cpsrc.txt)
[[ "$c" == 204 && "$b" == "copied" && "$co" == 404 && "$bs" == "copied" ]] \
    && pass "COPY onto an existing collection, Overwrite T -> 204, old tree gone, source kept" \
    || fail "COPY onto a collection (got $c, dest '$b', old $co, source '$bs'; want 204 'copied' 404 'copied')"

# Every overwrite above replaced its destination: none leaves its aside.
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
[[ "$r" == *"/dav/cpdst"* ]] && ! grep -qE '~AXL[0-9A-F]{4}\.TMP' <<< "$r" \
    && pass "a replaced destination leaves no aside behind" \
    || fail "an aside was left after a successful overwrite"

# A move that cannot happen once the destination is aside -- a collection
# onto an existing collection BELOW itself -- puts the destination back.
code MKCOL /dav/outer >/dev/null
code MKCOL /dav/outer/inner >/dev/null
put /dav/outer/inner/f.txt "f"
c=$(code MOVE /dav/outer -H "Destination: ${BASE}/dav/outer/inner" -H "Overwrite: T")
b=$(body /dav/outer/inner/f.txt)
[[ "$c" == 403 && "$b" == "f" ]] \
    && pass "MOVE a collection onto one below itself -> 403, nothing changed" \
    || fail "MOVE into its own subtree (got $c, inner file '$b'; want 403 'f')"

# A NO_DELETE mount refuses an overwrite that would remove a collection --
# MOVE and COPY cannot wipe a tree DELETE may not -- but still replaces a file.
code MKCOL /dav/ndcol >/dev/null
put /dav/ndcol/keep.txt "keep"
put /dav/nd.txt "nd"
c=$(code MOVE /nodel/nd.txt -H "Destination: ${BASE}/nodel/ndcol" -H "Overwrite: T")
b=$(body /dav/ndcol/keep.txt)
bs=$(body /dav/nd.txt)
[[ "$c" == 403 && "$b" == "keep" && "$bs" == "nd" ]] \
    && pass "NO_DELETE: MOVE over a collection -> 403, tree and source intact" \
    || fail "NO_DELETE MOVE over a collection (got $c, keep '$b', source '$bs'; want 403 'keep' 'nd')"
c=$(code COPY /nodel/nd.txt -H "Destination: ${BASE}/nodel/ndcol" -H "Overwrite: T")
b=$(body /dav/ndcol/keep.txt)
[[ "$c" == 403 && "$b" == "keep" ]] \
    && pass "NO_DELETE: COPY over a collection -> 403, tree intact" \
    || fail "NO_DELETE COPY over a collection (got $c, keep '$b'; want 403 'keep')"
put /dav/ndold.txt "old file"
c=$(code MOVE /nodel/nd.txt -H "Destination: ${BASE}/nodel/ndold.txt" -H "Overwrite: T")
b=$(body /dav/ndold.txt)
[[ "$c" == 204 && "$b" == "nd" ]] \
    && pass "NO_DELETE: MOVE over a FILE is still allowed -> 204" \
    || fail "NO_DELETE MOVE over a file (got $c, body '$b'; want 204 'nd')"

# Source and destination the same resource: 403 (RFC 4918 §9.9.4, §9.8.5).
put /dav/same.txt "same"
c=$(code MOVE /dav/same.txt -H "Destination: ${BASE}/dav/same.txt")
c2=$(code COPY /dav/same.txt -H "Destination: ${BASE}/dav/same.txt")
b=$(body /dav/same.txt)
[[ "$c" == 403 && "$c2" == 403 && "$b" == "same" ]] \
    && pass "MOVE and COPY onto themselves -> 403, file intact" \
    || fail "self MOVE/COPY (got $c / $c2, body '$b'; want 403 403 'same')"

# The mount root is never a destination: overwriting it is DELETE /.
c=$(code MOVE /dav/same.txt -H "Destination: ${BASE}/dav/")
b=$(body /dav/same.txt)
[[ "$c" == 403 && "$b" == "same" ]] \
    && pass "MOVE onto the mount root -> 403, source intact" \
    || fail "MOVE onto the mount root (got $c, body '$b'; want 403 'same')"

# A Destination on another server: 502 (RFC 4918 §9.9.4, §9.8.5).
c=$(code MOVE /dav/same.txt -H "Destination: http://other.invalid:9/dav/moved.txt")
c2=$(code COPY /dav/same.txt -H "Destination: http://other.invalid:9/dav/moved.txt")
b=$(body /dav/same.txt)
[[ "$c" == 502 && "$c2" == 502 && "$b" == "same" ]] \
    && pass "MOVE and COPY to another host -> 502, source intact" \
    || fail "MOVE/COPY to another host (got $c / $c2, body '$b'; want 502 502 'same')"

# The replaced destination cannot be removed whole (a read-only file is
# staged inside it): the MOVE has happened, so it is 204, and the leftover
# aside is logged by name rather than reported as a failed MOVE.
put /dav/lv.txt "lv"
c=$(code MOVE /dav/lv.txt -H "Destination: ${BASE}/dav/lockcol" -H "Overwrite: T")
b=$(body /dav/lockcol)
r=$(curl "${C[@]}" -X PROPFIND -H "Depth: 1" "${BASE}/dav/" 2>/dev/null || true)
aside=$(grep -oE '/dav/~AXL[0-9A-F]{4}\.TMP/' <<< "$r" | head -1 || true)
ro=$(body "${aside}ro.txt")
[[ "$c" == 204 && "$b" == "lv" && -n "$aside" && "$ro" == "locked" ]] \
    && pass "a destination that cannot be removed whole: 204, the leftover ${aside} kept" \
    || fail "partial clear (got $c, dest '$b', aside '${aside}', its file '$ro'; want 204 'lv' ~AXLhhhh.TMP 'locked')"
test_clean_log
an=${aside#/dav/}; an=${an%/}
grep -Eq "'[^']*davroot.${an//./\\.}' could not be removed; it holds the entry" "$TEST_CLEAN_LOG" \
    && pass "the leftover is logged by name" \
    || fail "no log line names the leftover '${an}'"

# 7) Traversal escape must be refused (no reading outside the subtree).
c=$(curl "${C[@]}" --path-as-is -o /dev/null -w '%{http_code}' "${BASE}/dav/../../axl_wr.tmp" 2>/dev/null || echo 000)
[[ "$c" != 200 ]] && pass "traversal escape refused (got $c, not 200)" || fail "traversal escaped (got 200)"

# 8) Read-only mount: GET works, PUT is 405.
c=$(code GET /ro/sub/moved.txt); [[ "$c" == 200 ]] && pass "readonly GET works" || fail "readonly GET (got $c)"
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE}/ro/nope.txt" <<< "x" 2>/dev/null || echo 000)
[[ "$c" == 405 ]] && pass "readonly PUT -> 405" || fail "readonly PUT (got $c, want 405)"

# 8b) Empty-body framing on a KEEP-ALIVE connection. An empty-body 201
#     (PUT/MKCOL) or 200 (OPTIONS) must carry Content-Length: 0, or a
#     keep-alive client blocks waiting for a body that never arrives.
#     NOTE: the `C` array above forces "Connection: close", which masks
#     this via EOF framing — these cases deliberately do NOT, so the bug
#     can't regress. A hang surfaces as curl rc=28 (timeout); we assert
#     completion (rc 0), not just the status code.
ka_code() {  # ka_code <curl args...> ; echo "<rc> <http_code>"
    local out rc
    # Capture curl's exit code (28 on a keep-alive hang) WITHOUT letting
    # set -e abort the script: the `|| rc=$?` arm absorbs the failure.
    out=$(curl -s --max-time 8 -o /dev/null -w '%{http_code}' "$@" 2>/dev/null) \
        && rc=0 || rc=$?
    echo "$rc ${out:-000}"
}
read -r rc c < <(ka_code -T - "${BASE}/dav/ka.txt" <<< "ka-body")
[[ "$rc" == 0 && "$c" == 2* ]] && pass "keep-alive PUT completes (rc=$rc, $c)" \
    || fail "keep-alive PUT hung/failed (rc=$rc, code=$c — missing Content-Length: 0?)"
read -r rc c < <(ka_code -X MKCOL "${BASE}/dav/kadir")
[[ "$rc" == 0 && "$c" == 2* ]] && pass "keep-alive MKCOL completes (rc=$rc, $c)" \
    || fail "keep-alive MKCOL hung/failed (rc=$rc, code=$c)"
read -r rc c < <(ka_code -X OPTIONS "${BASE}/dav/")
[[ "$rc" == 0 && "$c" == 2* ]] && pass "keep-alive OPTIONS completes (rc=$rc, $c)" \
    || fail "keep-alive OPTIONS hung/failed (rc=$rc, code=$c)"
# HEAD sets its OWN entity-length Content-Length with an empty body, so
# the auto-emit must NOT add a second (duplicate) Content-Length: 0. curl
# silently takes the first CL and reports rc=0/200 either way, so assert
# the header directly: exactly ONE Content-Length line, value != 0 (the
# real entity length). Remove the dup-guard in send_response and this sees
# two lines -> RED. (This is the regression guard for the HEAD path.)
hdr=$(curl -s -I --max-time 8 "${BASE}/dav/ka.txt" 2>/dev/null || true)
n=$(printf '%s' "$hdr" | grep -ci '^Content-Length:' || true)
cl=$(printf '%s' "$hdr" | grep -i '^Content-Length:' | head -1 | tr -d '\r' | awk '{print $2}')
[[ "$n" == 1 && -n "$cl" && "$cl" != 0 ]] \
    && pass "HEAD: single Content-Length=$cl (no duplicate, no zero-override)" \
    || fail "HEAD Content-Length wrong (count=$n value=${cl:-none} — duplicate or zero?)"

# 9) Auth-gated mount (/auth via serve_fs AXL_ROUTE_AUTH) — every verb,
#    including the streaming PUT, requires "Bearer test-token".
AUTH=(-H "Authorization: Bearer test-token")
ADMIN=(-H "Authorization: Bearer admin-token")

# 9a) Non-upload verbs gate via the dispatch auth check.
c=$(code PROPFIND /auth/ -H "Depth: 1"); [[ "$c" == 401 ]] && pass "PROPFIND /auth/ unauth -> 401" || fail "PROPFIND /auth/ unauth (got $c, want 401)"
c=$(code GET /auth/x.txt);               [[ "$c" == 401 ]] && pass "GET /auth unauth -> 401"        || fail "GET /auth unauth (got $c, want 401)"

# 9b) PUT rides the streaming upload path (bypasses dispatch_request) —
#     the gate must fire BEFORE any body byte. 401 without creds.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE}/auth/x.txt" <<< "secret" 2>/dev/null || echo 000)
[[ "$c" == 401 ]] && pass "PUT /auth unauth -> 401 (upload gated pre-body)" || fail "PUT /auth unauth (got $c, want 401)"

# 9c) With creds, the full flow works.
c=$(curl "${C[@]}" "${AUTH[@]}" -o /dev/null -w '%{http_code}' -T - "${BASE}/auth/x.txt" <<< "secret" 2>/dev/null || echo 000)
in2xx "$c" && pass "PUT /auth authed -> $c" || fail "PUT /auth authed (got $c)"
c=$(curl "${C[@]}" "${AUTH[@]}" -o /dev/null -w '%{http_code}' "${BASE}/auth/x.txt" 2>/dev/null || echo 000)
[[ "$c" == 200 ]] && pass "GET /auth authed -> 200" || fail "GET /auth authed (got $c)"
b=$(curl "${C[@]}" "${AUTH[@]}" "${BASE}/auth/x.txt" 2>/dev/null || true)
[[ "$b" == *"secret"* ]] && pass "GET /auth authed returns body" || fail "GET /auth authed body ('$b')"

# 10) Standalone add_upload_route_auth — 401 without creds, 2xx with.
c=$(curl "${C[@]}" -o /dev/null -w '%{http_code}' -X POST --data-binary "abc" "${BASE}/upload-auth" 2>/dev/null || echo 000)
[[ "$c" == 401 ]] && pass "POST /upload-auth unauth -> 401" || fail "POST /upload-auth unauth (got $c, want 401)"
c=$(curl "${C[@]}" "${AUTH[@]}" -o /dev/null -w '%{http_code}' -X POST --data-binary "abc" "${BASE}/upload-auth" 2>/dev/null || echo 000)
in2xx "$c" && pass "POST /upload-auth authed -> $c" || fail "POST /upload-auth authed (got $c)"

# 11) Admin upload route — authenticated-but-not-admin is 403; admin token works.
c=$(curl "${C[@]}" "${AUTH[@]}" -o /dev/null -w '%{http_code}' -X POST --data-binary "abc" "${BASE}/upload-admin" 2>/dev/null || echo 000)
[[ "$c" == 403 ]] && pass "POST /upload-admin non-admin -> 403" || fail "POST /upload-admin non-admin (got $c, want 403)"
c=$(curl "${C[@]}" "${ADMIN[@]}" -o /dev/null -w '%{http_code}' -X POST --data-binary "abc" "${BASE}/upload-admin" 2>/dev/null || echo 000)
in2xx "$c" && pass "POST /upload-admin admin -> $c" || fail "POST /upload-admin admin (got $c)"

# 12) A PUT that fails must not wedge the mount. The mount threads one
#     PUT's state across its chunk calls; a PUT whose target could not be
#     opened left that state marked failed, so the next PUT on the mount
#     opened -- TRUNCATED -- its target and was then answered 500, and it
#     kept the mount's single in-flight slot, so every PUT after it was
#     500 too. A failed PUT is its own answer: the next one is untouched.

c=$(putc /dav/precious.txt "precious v1")
b=$(body /dav/precious.txt)
[[ "$c" == 201 && "$b" == "precious v1" ]] \
    && pass "PUT a new file -> 201 with its body" \
    || fail "setup PUT (got $c, body '$b'; want 201 'precious v1')"
c=$(putc /dav/nodir/q.txt "orphan")
cg=$(code GET /dav/nodir/q.txt)
[[ "$c" == 409 && "$cg" == 404 ]] \
    && pass "PUT into a missing collection -> 409, nothing created" \
    || fail "PUT into a missing collection (got $c, GET $cg; want 409 404)"
c=$(putc /dav/precious.txt "precious v2")
b=$(body /dav/precious.txt)
[[ "$c" == 201 && "$b" == "precious v2" ]] \
    && pass "a PUT over an existing file right after a failed PUT replaces it" \
    || fail "PUT over an existing file after a failed PUT (got $c, body '$b'; want 201 'precious v2')"
c=$(putc /dav/after1.txt "after one")
b=$(body /dav/after1.txt)
[[ "$c" == 201 && "$b" == "after one" ]] \
    && pass "and a new file after it -> 201 with its body" \
    || fail "new-file PUT after a failed PUT (got $c, body '$b'; want 201 'after one')"
c=$(putc /dav/nodir/q.txt "orphan")
c2=$(putc /dav/after2.txt "after two")
b=$(body /dav/after2.txt)
[[ "$c" == 409 && "$c2" == 201 && "$b" == "after two" ]] \
    && pass "a second failure is just as contained -> 409 then 201" \
    || fail "PUT after a second failed PUT (got $c / $c2, body '$b'; want 409 201 'after two')"

webdav_finish
