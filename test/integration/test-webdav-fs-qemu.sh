#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=0
# test-webdav-fs-qemu.sh — axl_http_server_serve_fs end to end.
#
# Boots AxlTestNet.efi serve-davfs (an fs-backed WebDAV file server over a
# clean subtree of the boot volume: /dav read-write, /ro read-only) and
# drives the full verb set from the host with curl: PUT (streaming write
# via AxlFileWriter), GET (streaming read via AxlFileView), PROPFIND,
# MKCOL, MOVE, COPY, DELETE — plus a traversal-escape rejection and a
# read-only 405. Proves the generic AxlWebDavOps glue + AxlFileWriter.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# Usage: ./test/integration/test-webdav-fs-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
test_add_efi "$PROJECT_DIR/out/native-$_native_arch/AxlTestNet.efi"

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

test_build_image

echo "=== AxlNet serve_fs WebDAV Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID  (host $HOST_PORT -> $GUEST_PORT)"
if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not start within 60 seconds"
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
    curl "${C[@]}" -o /dev/null -w '%{http_code}' -X "$m" "$@" "${BASE}${path}" 2>/dev/null || echo 000
}
# body <path>
body() { curl "${C[@]}" "${BASE}$1" 2>/dev/null || true; }
in2xx() { [[ "$1" == 2* ]]; }

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

echo ""
echo "  serve_fs WebDAV: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ "$FAIL" -eq 0 ]] || exit 1
exit 0
