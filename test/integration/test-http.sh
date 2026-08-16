#!/bin/bash
# test-meta: arch=x64 needs= est=35 local-only=0
# AxlNet HTTP server integration test — boots QEMU with port forwarding,
# starts the HTTP server inside UEFI, validates with curl from the host.
#
# Usage: ./test/integration/test-http.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

# Stage test app
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"
test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Startup script: init network, run DHCP, start HTTP server
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

echo Network status:
ifconfig -l
stall 1000000

echo Starting HTTP server...
AxlTestNet.efi serve
NSHEOF

test_build_image

echo "=== AxlNet HTTP Integration Test ($TEST_ARCH) ==="

# ---------------------------------------------------------------------------
# Start a Python HTTP server on the host for UEFI client tests.
# The UEFI guest reaches it via QEMU gateway at 10.0.2.2.
# ---------------------------------------------------------------------------

HOST_SERVER_PORT=$(test_port 1)
HOST_SERVER_PID=0

python3 "$(dirname "$0")/host-server.py" "$HOST_SERVER_PORT" &
HOST_SERVER_PID=$!

trap 'test_cleanup; [[ $HOST_SERVER_PID -gt 0 ]] && kill $HOST_SERVER_PID 2>/dev/null || true' EXIT

sleep 1
echo "  Host HTTP server PID: $HOST_SERVER_PID, port: $HOST_SERVER_PORT"

# Boot QEMU with networking and port forwarding
test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTP server..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: HTTP server did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"
    tail -20 "$TEST_CLEAN_LOG"
    echo "---"
    exit 1
fi

echo "  Server is ready"
sleep 2

# ---------------------------------------------------------------------------
# HTTP tests via curl
# ---------------------------------------------------------------------------

PASS=0
FAIL=0
BASE_URL="http://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s -H "Connection: close" --max-time 10)

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

http_get() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}$1" 2>/dev/null || true
}

http_post() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "$2" "${BASE_URL}$1" 2>/dev/null || true
}

echo ""
echo "  --- HTTP Server Tests ---"

# GET /api/version — JSON response
RESP=$(http_get "/api/version")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /api/version returns 200" || fail "GET /api/version (got $CODE)"
echo "$BODY" | grep -q '"version"' && pass "/api/version body has version" || fail "/api/version missing version"

# GET /api/health — JSON health check
RESP=$(http_get "/api/health")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /api/health returns 200" || fail "GET /api/health (got $CODE)"
echo "$BODY" | grep -q '"status"' && pass "/api/health body has status" || fail "/api/health missing status"

# GET /plain — text response
RESP=$(http_get "/plain")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /plain returns 200" || fail "GET /plain (got $CODE)"
echo "$BODY" | grep -q "Hello from AxlNet" && pass "/plain has expected text" || fail "/plain wrong body"

# POST /echo — echo body back
RESP=$(http_post "/echo" "test payload")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /echo returns 200" || fail "POST /echo (got $CODE)"
echo "$BODY" | grep -q "test payload" && pass "/echo returns posted body" || fail "/echo wrong body"

# GET /static-asset twice — regression for the body_static contract.
# Handler returns the same .rodata buffer via axl_http_response_set_static.
# Pre-fix, the dispatch loop axl_free()'d the .rodata pointer after the
# first send (heap corruption); the second curl typically hung mid-stream
# or returned truncated data (curl exit 56). Both responses must be
# byte-identical and 200 OK.
RESP=$(http_get "/static-asset")
CODE=$(echo "$RESP" | tail -1)
BODY1=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /static-asset (1st) returns 200" || fail "GET /static-asset 1st (got $CODE)"
echo "$BODY1" | grep -q "embedded asset" && pass "/static-asset 1st has expected body" || fail "/static-asset 1st wrong body"

RESP=$(http_get "/static-asset")
CODE=$(echo "$RESP" | tail -1)
BODY2=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /static-asset (2nd) returns 200" || fail "GET /static-asset 2nd (got $CODE)"
echo "$BODY2" | grep -q "embedded asset" && pass "/static-asset 2nd has expected body" || fail "/static-asset 2nd wrong body"
[[ "$BODY1" == "$BODY2" ]] && pass "/static-asset is byte-identical across requests (no free)" \
                          || fail "/static-asset bodies diverged (heap corruption symptom)"

# GET /nonexistent — 404
RESP=$(http_get "/nonexistent")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "404" ]] && pass "GET /nonexistent returns 404" || fail "GET /nonexistent (got $CODE)"

# ---------------------------------------------------------------------------
# Route lookup: exact vs prefix
#
# /rt/         and /rt/*         are both registered. Same for
# /rt/files    and /rt/files/*.  Verify the exact route always wins at
# its own path and the prefix takes over at any sub-path. This
# regression-tests the split-tree fix where exact and prefix routes
# previously collided on the same radix key.
# ---------------------------------------------------------------------------

# Exact / prefix at the same level
RESP=$(http_get "/rt/")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/ returns 200" || fail "GET /rt/ (got $CODE)"
[[ "$BODY" == "exact" ]] && pass "GET /rt/ matches exact route" || fail "GET /rt/ wrong handler ($BODY)"

RESP=$(http_get "/rt/anything")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/anything returns 200" || fail "GET /rt/anything (got $CODE)"
[[ "$BODY" == "prefix" ]] && pass "GET /rt/anything matches prefix route" || fail "GET /rt/anything wrong handler ($BODY)"

RESP=$(http_get "/rt/a/b/c")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/a/b/c returns 200" || fail "GET /rt/a/b/c (got $CODE)"
[[ "$BODY" == "prefix" ]] && pass "GET /rt/a/b/c matches prefix route" || fail "GET /rt/a/b/c wrong handler ($BODY)"

# Nested exact / prefix — longest-prefix wins, exact still beats it at the same path
RESP=$(http_get "/rt/files")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/files returns 200" || fail "GET /rt/files (got $CODE)"
[[ "$BODY" == "nested-exact" ]] && pass "GET /rt/files matches nested exact" || fail "GET /rt/files wrong handler ($BODY)"

RESP=$(http_get "/rt/files/foo.txt")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /rt/files/foo.txt returns 200" || fail "GET /rt/files/foo.txt (got $CODE)"
[[ "$BODY" == "nested-prefix" ]] && pass "GET /rt/files/foo.txt matches nested prefix" || fail "GET /rt/files/foo.txt wrong handler ($BODY)"

# Content-Type header check
CT=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{content_type}" "${BASE_URL}/api/version" 2>/dev/null || true)
echo "$CT" | grep -qi "application/json" && pass "Content-Type is application/json" || fail "Content-Type is '$CT'"

# ---------------------------------------------------------------------------
# Authentication tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- Authentication Tests ---"

# GET /secret without credentials → 401
RESP=$(http_get "/secret")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "401" ]] && pass "GET /secret no auth returns 401" || fail "GET /secret no auth (got $CODE)"

# The 401 carries a WWW-Authenticate challenge (set via
# axl_http_server_set_auth_challenge) so browsers/Finder prompt for creds.
HDRS=$(curl "${CURL_OPTS[@]}" -i "${BASE_URL}/secret" 2>/dev/null || true)
echo "$HDRS" | grep -qi 'WWW-Authenticate: Basic realm="axl-test"' \
    && pass "GET /secret 401 carries WWW-Authenticate challenge" \
    || fail "GET /secret 401 missing WWW-Authenticate challenge"

# GET /secret with wrong token → 401
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -H "Authorization: Bearer wrong" "${BASE_URL}/secret" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "401" ]] && pass "GET /secret wrong token returns 401" || fail "GET /secret wrong token (got $CODE)"

# GET /secret with valid token → 200
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -H "Authorization: Bearer test-token" "${BASE_URL}/secret" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /secret valid token returns 200" || fail "GET /secret valid token (got $CODE)"
echo "$BODY" | grep -q '"secret"' && pass "/secret has expected body" || fail "/secret wrong body"

# ---------------------------------------------------------------------------
# Response caching tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- Response Caching Tests ---"

# First request — handler runs, count=1
RESP=$(http_get "/cached")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /cached first request returns 200" || fail "GET /cached first (got $CODE)"
echo "$BODY" | grep -q '"count":1' && pass "/cached first request count=1" || fail "/cached first count wrong ($BODY)"

# Second request — should be cached, still count=1
RESP=$(http_get "/cached")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /cached second request returns 200" || fail "GET /cached second (got $CODE)"
echo "$BODY" | grep -q '"count":1' && pass "/cached second request still count=1 (cached)" || fail "/cached second count changed ($BODY)"

# ---------------------------------------------------------------------------
# cache_max FIFO eviction — server is configured with use_cache(s, 3).
# Walk four distinct paths /cm/1../cm/4; after /cm/4 the oldest entry
# (/cm/1) must have been evicted, and a fresh GET /cm/1 should re-run
# the handler (count incremented).
# ---------------------------------------------------------------------------

echo ""
echo "  --- Cache Eviction Tests (cache_max) ---"

RESP=$(http_get "/cm/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/1 first request count=1" || fail "/cm/1 first count wrong ($BODY)"

RESP=$(http_get "/cm/2")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/2 first request count=1" || fail "/cm/2 first count wrong ($BODY)"

RESP=$(http_get "/cm/3")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/3 first request count=1" || fail "/cm/3 first count wrong ($BODY)"

# cm/1 should still be in cache at this point (size 3 = max, nothing forced eviction yet)
RESP=$(http_get "/cm/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/1 still cached (count=1)" || fail "/cm/1 unexpectedly re-ran ($BODY)"

# /cm/4 pushes cache over limit — oldest (/cm/1) is evicted
RESP=$(http_get "/cm/4")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/cm/4 first request count=1" || fail "/cm/4 first count wrong ($BODY)"

# /cm/1 should now be a miss — handler re-runs
RESP=$(http_get "/cm/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/cm/1 evicted and re-run (count=2)" || fail "/cm/1 eviction failed ($BODY)"

# ---------------------------------------------------------------------------
# Per-route TTL — /ttl-short has a 1.5s TTL, /ttl-long uses the
# server-wide 60-second default. After sleeping 2s, /ttl-short
# must expire (handler re-runs) while /ttl-long stays cached.
# (TTL must exceed UEFI's 1-second clock granularity so that
# `now - timestamp_ms` is non-zero on the second request.)
# ---------------------------------------------------------------------------

echo ""
echo "  --- Per-route TTL Tests ---"

RESP=$(http_get "/ttl-short")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/ttl-short first request count=1" || fail "/ttl-short first count wrong ($BODY)"

RESP=$(http_get "/ttl-long")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/ttl-long first request count=1" || fail "/ttl-long first count wrong ($BODY)"

sleep 2

RESP=$(http_get "/ttl-short")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/ttl-short expired and re-run (count=2)" || fail "/ttl-short TTL not honored ($BODY)"

RESP=$(http_get "/ttl-long")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/ttl-long still cached (count=1)" || fail "/ttl-long unexpectedly re-ran ($BODY)"

# ---------------------------------------------------------------------------
# Prefix-based cache invalidation — after caching three entries under
# /api/users/* and /api/posts/1, GET /invalidate-users calls
# axl_http_server_cache_invalidate(s, "/api/users"), which must
# remove both /api/users/* entries and leave /api/posts/1 intact.
# ---------------------------------------------------------------------------

echo ""
echo "  --- Prefix Cache Invalidation Tests ---"

RESP=$(http_get "/api/users/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/users/1 first request count=1" || fail "/api/users/1 first count wrong ($BODY)"

RESP=$(http_get "/api/users/2")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/users/2 first request count=1" || fail "/api/users/2 first count wrong ($BODY)"

RESP=$(http_get "/api/posts/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/posts/1 first request count=1" || fail "/api/posts/1 first count wrong ($BODY)"

# Sanity: /api/users/1 should still be cached
RESP=$(http_get "/api/users/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/users/1 cached before invalidate" || fail "/api/users/1 not cached ($BODY)"

# Trigger invalidate
RESP=$(http_get "/invalidate-users")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "204" ]] && pass "/invalidate-users returns 204" || fail "/invalidate-users wrong code ($CODE)"

# /api/users/* should now miss and re-run
RESP=$(http_get "/api/users/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/api/users/1 invalidated (count=2)" || fail "/api/users/1 still cached ($BODY)"

RESP=$(http_get "/api/users/2")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":2' && pass "/api/users/2 invalidated (count=2)" || fail "/api/users/2 still cached ($BODY)"

# /api/posts/1 should still be cached (prefix didn't match)
RESP=$(http_get "/api/posts/1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"count":1' && pass "/api/posts/1 still cached after invalidate" || fail "/api/posts/1 wrongly invalidated ($BODY)"

# ---------------------------------------------------------------------------
# Upload streaming tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- Upload Streaming Tests ---"

# Small upload (fits in one chunk with 1024 chunk size)
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "hello" "${BASE_URL}/upload" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /upload small returns 200" || fail "POST /upload small (got $CODE)"
echo "$BODY" | grep -q '"chunks":1' && pass "/upload small: 1 chunk" || fail "/upload small chunks (body: $BODY)"
echo "$BODY" | grep -q '"total":5' && pass "/upload small: 5 bytes total" || fail "/upload small total (body: $BODY)"

# Large upload (4KB > 1024 chunk size, so multiple chunks expected)
LARGE_DATA=$(python3 -c "print('X' * 4096, end='')")
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "$LARGE_DATA" "${BASE_URL}/upload" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /upload large returns 200" || fail "POST /upload large (got $CODE)"
echo "$BODY" | grep -qE '"chunks":[2-9]' && pass "/upload large: multiple chunks" || fail "/upload large chunks (body: $BODY)"
echo "$BODY" | grep -q '"total":4096' && pass "/upload large: 4096 bytes total" || fail "/upload large total (body: $BODY)"

# ---------------------------------------------------------------------------
# Middleware runs ahead of upload routes (regression: silent bypass).
# Pre-fix: handler ran, returned 200 with chunks/total, /upload-status
# chunks_lifetime advanced. Post-fix: middleware short-circuits with
# 403 before the handler sees a single byte; chunks_lifetime stays flat.
# ---------------------------------------------------------------------------

# Snapshot lifetime chunk count before the rejected upload. Query
# string is a unique nonce so the response cache (60 s default TTL)
# can't serve a stale body — every snapshot must be fresh.
RESP=$(http_get "/upload-status?seq=mw-before")
BEFORE_BODY=$(echo "$RESP" | sed '$d')
BEFORE_CHUNKS=$(echo "$BEFORE_BODY" | sed -n 's/.*"chunks_lifetime":\([0-9]*\).*/\1/p')

RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST \
    -H "X-Test-Reject: 1" -d "should-not-arrive" \
    "${BASE_URL}/upload" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "403" ]] && pass "POST /upload + X-Test-Reject returns 403" \
                       || fail "/upload middleware (got $CODE, body: $BODY)"
echo "$BODY" | grep -q "rejected by middleware" \
    && pass "/upload middleware: 403 body comes from middleware" \
    || fail "/upload middleware: wrong body ($BODY)"

# Verify the upload handler didn't run at all (fresh cache key again).
RESP=$(http_get "/upload-status?seq=mw-after")
AFTER_BODY=$(echo "$RESP" | sed '$d')
AFTER_CHUNKS=$(echo "$AFTER_BODY" | sed -n 's/.*"chunks_lifetime":\([0-9]*\).*/\1/p')
[[ -n "$BEFORE_CHUNKS" && "$BEFORE_CHUNKS" == "$AFTER_CHUNKS" ]] \
    && pass "/upload middleware: handler never invoked (chunks_lifetime flat at $AFTER_CHUNKS)" \
    || fail "/upload middleware bypassed (chunks_lifetime $BEFORE_CHUNKS -> $AFTER_CHUNKS, body: $AFTER_BODY)"

# ---------------------------------------------------------------------------
# WebDAV class-1 + MOVE (W1: OPTIONS / MKCOL / DELETE / MOVE)
# Test backend is the in-memory hash-table fs in axl-test-net.c.
# ---------------------------------------------------------------------------

echo ""
echo "  --- WebDAV W1 (OPTIONS / MKCOL / DELETE / MOVE) ---"

# OPTIONS — must include DAV: 1 and an Allow header listing the verbs.
RESP=$(curl "${CURL_OPTS[@]}" -i -X OPTIONS "${BASE_URL}/dav/" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 200 " \
    && pass "OPTIONS /dav/ returns 200" \
    || fail "OPTIONS /dav/ status (got: $(echo "$RESP" | head -1))"
echo "$RESP" | grep -qi "^DAV:.*1" \
    && pass "OPTIONS /dav/ advertises DAV: 1" \
    || fail "OPTIONS /dav/ missing DAV: 1 header"
echo "$RESP" | grep -qi "^Allow:.*PROPFIND" \
    && pass "OPTIONS /dav/ Allow header lists PROPFIND" \
    || fail "OPTIONS /dav/ Allow missing PROPFIND"
echo "$RESP" | grep -qi "^Allow:.*MOVE" \
    && pass "OPTIONS /dav/ Allow header lists MOVE" \
    || fail "OPTIONS /dav/ Allow missing MOVE"
echo "$RESP" | grep -qi "^Allow:.*COPY" \
    && pass "OPTIONS /dav/ Allow header lists COPY" \
    || fail "OPTIONS /dav/ Allow missing COPY"

# MKCOL — create a new collection. 201 on success, 409 if it already exists.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MKCOL "${BASE_URL}/dav/newdir" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "MKCOL /dav/newdir returns 201" \
                       || fail "MKCOL /dav/newdir (got $CODE)"

# Second MKCOL on same path: backend rejects → 409.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MKCOL "${BASE_URL}/dav/newdir" 2>/dev/null || true)
[[ "$CODE" == "409" ]] && pass "MKCOL /dav/newdir (already exists) returns 409" \
                       || fail "MKCOL repeat (got $CODE)"

# MKCOL on root — refused (the mount root always exists). 405.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MKCOL "${BASE_URL}/dav/" 2>/dev/null || true)
[[ "$CODE" == "405" ]] && pass "MKCOL /dav/ (root) refused with 405" \
                       || fail "MKCOL / (got $CODE)"

# MKCOL with a body — RFC 4918 §9.3.1: 415 Unsupported Media Type.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MKCOL -d "<unsupported/>" "${BASE_URL}/dav/with-body" 2>/dev/null || true)
[[ "$CODE" == "415" ]] && pass "MKCOL with body returns 415" \
                       || fail "MKCOL with body (got $CODE)"

# DELETE — remove a known entry. 204 on success.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X DELETE "${BASE_URL}/dav/preset-file" 2>/dev/null || true)
[[ "$CODE" == "204" ]] && pass "DELETE /dav/preset-file returns 204" \
                       || fail "DELETE /dav/preset-file (got $CODE)"

# DELETE on missing entry → 404.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X DELETE "${BASE_URL}/dav/missing" 2>/dev/null || true)
[[ "$CODE" == "404" ]] && pass "DELETE /dav/missing returns 404" \
                       || fail "DELETE /dav/missing (got $CODE)"

# DELETE / — refused (would destroy the mount). 403.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X DELETE "${BASE_URL}/dav/" 2>/dev/null || true)
[[ "$CODE" == "403" ]] && pass "DELETE /dav/ (root) refused with 403" \
                       || fail "DELETE / (got $CODE)"

# MOVE — Destination header parsing + relative-path resolution.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MOVE -H "Destination: ${BASE_URL}/dav/moved-dir" \
    "${BASE_URL}/dav/preset-dir" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "MOVE /dav/preset-dir -> /dav/moved-dir returns 201" \
                       || fail "MOVE preset-dir (got $CODE)"

# PROPFIND on a known file (Depth: 0) — 207 Multi-Status, XML body
# carrying one <D:response> for the file itself.
RESP=$(curl "${CURL_OPTS[@]}" -i -X PROPFIND -H "Depth: 0" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 207 " \
    && pass "PROPFIND /dav/preset-stat (Depth:0) returns 207" \
    || fail "PROPFIND status (got: $(echo "$RESP" | head -1))"
echo "$RESP" | grep -qi "^Content-Type: application/xml" \
    && pass "PROPFIND emits Content-Type: application/xml" \
    || fail "PROPFIND missing application/xml content-type"
echo "$RESP" | grep -q '<D:multistatus' \
    && pass "PROPFIND body has <D:multistatus> root" \
    || fail "PROPFIND missing multistatus"
echo "$RESP" | grep -q '<D:href>/dav/preset-stat</D:href>' \
    && pass "PROPFIND href is prefixed correctly" \
    || fail "PROPFIND wrong href"
echo "$RESP" | grep -q '<D:getcontentlength>42</D:getcontentlength>' \
    && pass "PROPFIND emits getcontentlength" \
    || fail "PROPFIND missing getcontentlength"

# PROPFIND on a directory (Depth: 1) — self entry + children.
RESP=$(curl "${CURL_OPTS[@]}" -X PROPFIND -H "Depth: 1" \
    "${BASE_URL}/dav/" 2>/dev/null || true)
echo "$RESP" | grep -q '<D:href>/dav/</D:href>' \
    && pass "PROPFIND root self-entry has trailing slash" \
    || fail "PROPFIND root href"
echo "$RESP" | grep -q '<D:resourcetype><D:collection/></D:resourcetype>' \
    && pass "PROPFIND emits collection resourcetype for dirs" \
    || fail "PROPFIND missing collection"

# PROPFIND on a non-root collection WITH trailing slash — clients
# (Finder, cadaver, rclone) all do this. Pre-fix the SDK passed
# rel="/preset-dir/" to the consumer ops AND emitted hrefs with
# "//" double-slashes. Verify normalization: 207 + no // in any href.
RESP=$(curl "${CURL_OPTS[@]}" -i -X PROPFIND -H "Depth: 1" \
    "${BASE_URL}/dav/preset-collection/" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 207 " \
    && pass "PROPFIND /dav/preset-collection/ (trailing slash) returns 207" \
    || fail "PROPFIND /dav/preset-collection/ (got: $(echo "$RESP" | head -1))"
! echo "$RESP" | grep -q '<D:href>[^<]*//[^<]*</D:href>' \
    && pass "PROPFIND collection href has no // (rel normalized)" \
    || fail "PROPFIND emits double-slash hrefs"
echo "$RESP" | grep -q '<D:href>/dav/preset-collection/</D:href>' \
    && pass "PROPFIND collection self-href ends with single /" \
    || fail "PROPFIND collection href shape"

# PROPFIND on missing path → 404 (not 207 with empty multistatus).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND "${BASE_URL}/dav/missing" 2>/dev/null || true)
[[ "$CODE" == "404" ]] && pass "PROPFIND /dav/missing returns 404" \
                       || fail "PROPFIND missing (got $CODE)"

# Verify the XML well-formedness via Python (catches missing
# closing tags, unescaped entities, etc.).
RESP=$(curl "${CURL_OPTS[@]}" -X PROPFIND -H "Depth: 1" \
    "${BASE_URL}/dav/" 2>/dev/null || true)
echo "$RESP" | python3 -c "
import sys, xml.etree.ElementTree as ET
try:
    root = ET.fromstring(sys.stdin.read())
    ns = {'D': 'DAV:'}
    responses = root.findall('D:response', ns)
    if len(responses) >= 1:
        print('PASS: PROPFIND XML parses + has at least one response')
    else:
        print('FAIL: PROPFIND XML parsed but no responses')
except ET.ParseError as e:
    print(f'FAIL: PROPFIND XML well-formedness ({e})')
" | while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
    esac
done

# --- WebDAV W3: GET / HEAD / PUT (streaming) ---

# GET — full body. preset-stat backend returns DAV_FILE_BODY.
RESP=$(curl "${CURL_OPTS[@]}" -i "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 200 " \
    && pass "GET /dav/preset-stat returns 200" \
    || fail "GET /dav/preset-stat (got: $(echo "$RESP" | head -1))"
echo "$RESP" | grep -q "this is the contents of preset-stat" \
    && pass "GET body matches DAV_FILE_BODY" \
    || fail "GET body wrong"
echo "$RESP" | grep -qi "^Last-Modified: " \
    && pass "GET emits Last-Modified" \
    || fail "GET missing Last-Modified"

# GET with Range — partial content.
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Range: bytes=8-22" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 206 " \
    && pass "GET Range bytes=8-22 returns 206" \
    || fail "GET Range status"
echo "$RESP" | grep -qi "^Content-Range: bytes 8-22/" \
    && pass "GET Range emits Content-Range" \
    || fail "GET missing Content-Range"

# GET with unsatisfiable Range — RFC 7233 §4.4: 416 + Content-Range:
# bytes */<size>. Pre-fix returned 200 with the full body (parse_range
# returns false for both malformed AND unsatisfiable; the WebDAV
# handler now distinguishes via a leading-byte check).
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Range: bytes=99999-" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 416 " \
    && pass "GET unsatisfiable Range returns 416" \
    || fail "GET unsatisfiable Range (got: $(echo "$RESP" | head -1))"
echo "$RESP" | grep -qi "^Content-Range: bytes \*/" \
    && pass "GET 416 emits Content-Range: bytes */<size>" \
    || fail "GET 416 missing Content-Range */<size>"

# GET on directory → 405.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    "${BASE_URL}/dav/preset-collection" 2>/dev/null || true)
[[ "$CODE" == "405" ]] && pass "GET /dav/preset-collection (dir) returns 405" \
                       || fail "GET dir (got $CODE)"

# GET on missing → 404.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    "${BASE_URL}/dav/missing-get" 2>/dev/null || true)
[[ "$CODE" == "404" ]] && pass "GET /dav/missing-get returns 404" \
                       || fail "GET missing (got $CODE)"

# HEAD — same headers as GET, no body.
RESP=$(curl "${CURL_OPTS[@]}" -i -X HEAD "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 200 " \
    && pass "HEAD /dav/preset-stat returns 200" \
    || fail "HEAD status"
echo "$RESP" | grep -qi "^Content-Length: " \
    && pass "HEAD emits Content-Length" \
    || fail "HEAD missing Content-Length"
echo "$RESP" | grep -qi "^DAV: 1" \
    && pass "HEAD emits DAV: 1" \
    || fail "HEAD missing DAV header"

# PUT — small body. Verify the SDK delivered chunks to the test
# backend's write callbacks via /dav-status.
PUT_BODY="hello-from-PUT-test-v1"
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -d "$PUT_BODY" "${BASE_URL}/dav/uploaded.txt" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "PUT /dav/uploaded.txt returns 201" \
                       || fail "PUT (got $CODE)"

# Read /dav-status — verify path + len + body preview match.
RESP=$(http_get "/dav-status?seq=put1")
BODY=$(echo "$RESP" | sed '$d')
echo "$BODY" | grep -q '"path":"/uploaded.txt"' \
    && pass "PUT delivered correct rel path to write_open" \
    || fail "PUT path (body: $BODY)"
echo "$BODY" | grep -q "\"len\":${#PUT_BODY}" \
    && pass "PUT delivered correct byte count" \
    || fail "PUT len (body: $BODY)"
echo "$BODY" | grep -q "\"preview\":\"${PUT_BODY}\"" \
    && pass "PUT body preview matches" \
    || fail "PUT preview (body: $BODY)"
echo "$BODY" | grep -q '"aborted":false' \
    && pass "PUT delivered clean EOF (aborted=false)" \
    || fail "PUT aborted flag (body: $BODY)"

# After PUT, GET should see the new entry as a known file (404 → not 404
# is the cheap regression — the test backend registered it on
# write_close, so PROPFIND should now stat it).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND -H "Depth: 0" "${BASE_URL}/dav/uploaded.txt" 2>/dev/null || true)
[[ "$CODE" == "207" ]] && pass "PROPFIND /dav/uploaded.txt (post-PUT) returns 207" \
                       || fail "post-PUT PROPFIND (got $CODE)"

# PUT empty body — open + close to materialize empty file.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Length: 0" --data-binary "" \
    "${BASE_URL}/dav/empty.txt" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "PUT empty body returns 201" \
                       || fail "PUT empty (got $CODE)"

# PUT whose final flush/close FAILS. Every chunk was accepted, so nothing
# sets put_failed along the way — the only signal is write_close's status.
# The response must be 500: a 201 here tells the client its data is stored
# when the backend could not make it durable (full volume, write-protected
# media, device error). The test backend fails write_close for any target
# whose name starts with "flush-fails"; nothing else about the upload
# differs from the 201 case above.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -d "never-lands" "${BASE_URL}/dav/flush-fails.bin" 2>/dev/null || true)
[[ "$CODE" == "500" ]] && pass "PUT whose final flush fails returns 500, not 201" \
                       || fail "PUT flush-failure (got $CODE, want 500)"

# Same contract on the EMPTY-body PUT path — it closes through a separate
# call site, and 6a's lesson was that one side of a pair gets missed.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Length: 0" --data-binary "" \
    "${BASE_URL}/dav/flush-fails-empty.bin" 2>/dev/null || true)
[[ "$CODE" == "500" ]] && pass "empty-body PUT whose close fails returns 500" \
                       || fail "empty PUT flush-failure (got $CODE, want 500)"

# MOVE without Destination → 400.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MOVE "${BASE_URL}/dav/moved-dir" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "MOVE without Destination returns 400" \
                       || fail "MOVE no-dest (got $CODE)"

# MOVE with cross-prefix Destination → 400 (the SDK refuses to route
# moves outside its mount).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X MOVE -H "Destination: ${BASE_URL}/elsewhere/foo" \
    "${BASE_URL}/dav/moved-dir" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "MOVE cross-prefix Destination returns 400" \
                       || fail "MOVE cross-prefix (got $CODE)"

# --- WebDAV W5: COPY (RFC 4918 §9.8) ---

# COPY — Destination header + Overwrite default true. Source must
# survive the operation (the distinguishing trait vs MOVE).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY -H "Destination: ${BASE_URL}/dav/copied-file" \
    "${BASE_URL}/dav/copy-source-file" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "COPY /dav/copy-source-file -> /dav/copied-file returns 201" \
                       || fail "COPY basic (got $CODE)"

# Source must still exist after COPY — PROPFIND it.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND -H "Depth: 0" \
    "${BASE_URL}/dav/copy-source-file" 2>/dev/null || true)
[[ "$CODE" == "207" ]] && pass "COPY left source in place (PROPFIND src = 207)" \
                       || fail "COPY destroyed source (got $CODE)"

# Destination must exist after COPY — PROPFIND it.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND -H "Depth: 0" \
    "${BASE_URL}/dav/copied-file" 2>/dev/null || true)
[[ "$CODE" == "207" ]] && pass "COPY created destination (PROPFIND dst = 207)" \
                       || fail "COPY no destination (got $CODE)"

# COPY missing source → 404 (SDK pre-stats so this is distinguishable
# from the generic op-failure 409).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY -H "Destination: ${BASE_URL}/dav/copy-target-x" \
    "${BASE_URL}/dav/missing-source" 2>/dev/null || true)
[[ "$CODE" == "404" ]] && pass "COPY of missing source returns 404" \
                       || fail "COPY missing src (got $CODE)"

# COPY without Destination → 400.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY "${BASE_URL}/dav/copy-source-file" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "COPY without Destination returns 400" \
                       || fail "COPY no-dest (got $CODE)"

# COPY with cross-prefix Destination → 400.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY -H "Destination: ${BASE_URL}/elsewhere/foo" \
    "${BASE_URL}/dav/copy-source-file" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "COPY cross-prefix Destination returns 400" \
                       || fail "COPY cross-prefix (got $CODE)"

# COPY with Overwrite: F into an existing target → 409 (v1 lumps
# precondition failures into 409, matching MOVE; spec-strict 412
# is a known follow-up).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY -H "Destination: ${BASE_URL}/dav/copy-existing-target" \
    -H "Overwrite: F" \
    "${BASE_URL}/dav/copy-source-file" 2>/dev/null || true)
[[ "$CODE" == "409" ]] && pass "COPY Overwrite:F into existing target returns 409" \
                       || fail "COPY no-overwrite collision (got $CODE)"

# COPY of the mount root → 403 (symmetric with MOVE / DELETE /).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY -H "Destination: ${BASE_URL}/dav/root-copy" \
    "${BASE_URL}/dav/" 2>/dev/null || true)
[[ "$CODE" == "403" ]] && pass "COPY /dav/ (root) refused with 403" \
                       || fail "COPY root (got $CODE)"

# COPY with Depth: 1 → 400 (RFC 4918 §9.8.3 allows only "0" or
# "infinity"; "1" is invalid for COPY).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X COPY -H "Destination: ${BASE_URL}/dav/copy-depth1" \
    -H "Depth: 1" \
    "${BASE_URL}/dav/copy-source-file" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "COPY Depth:1 returns 400" \
                       || fail "COPY Depth:1 (got $CODE)"

# --- WebDAV W7a: before_response hook fires on every verb ---
# Test backend's before_response stamps every WebDAV response with
# `X-Test-Dav-Hook: <method>`. Hit each verb path; the header (and
# its method-echo value) must appear in every response.

# OPTIONS
RESP=$(curl "${CURL_OPTS[@]}" -i -X OPTIONS "${BASE_URL}/dav/" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: OPTIONS" \
    && pass "before_response: OPTIONS hook fired (method echoed)" \
    || fail "before_response: OPTIONS hook missing"

# PROPFIND
RESP=$(curl "${CURL_OPTS[@]}" -i -X PROPFIND -H "Depth: 0" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: PROPFIND" \
    && pass "before_response: PROPFIND hook fired" \
    || fail "before_response: PROPFIND hook missing"

# GET
RESP=$(curl "${CURL_OPTS[@]}" -i "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: GET" \
    && pass "before_response: GET hook fired" \
    || fail "before_response: GET hook missing"

# HEAD
RESP=$(curl "${CURL_OPTS[@]}" -i -X HEAD "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: HEAD" \
    && pass "before_response: HEAD hook fired" \
    || fail "before_response: HEAD hook missing"

# PUT — hook fires once at clean EOF, not per chunk.
RESP=$(curl "${CURL_OPTS[@]}" -i -X PUT -d "hook-test" \
    "${BASE_URL}/dav/hook-test.txt" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: PUT" \
    && pass "before_response: PUT hook fired at clean EOF" \
    || fail "before_response: PUT hook missing"

# MKCOL
RESP=$(curl "${CURL_OPTS[@]}" -i -X MKCOL \
    "${BASE_URL}/dav/hook-mkcol" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: MKCOL" \
    && pass "before_response: MKCOL hook fired" \
    || fail "before_response: MKCOL hook missing"

# DELETE
RESP=$(curl "${CURL_OPTS[@]}" -i -X DELETE \
    "${BASE_URL}/dav/hook-mkcol" 2>/dev/null || true)
echo "$RESP" | grep -qi "^X-Test-Dav-Hook: DELETE" \
    && pass "before_response: DELETE hook fired" \
    || fail "before_response: DELETE hook missing"

# --- WebDAV W7: Digest emission (RFC 3230) ---

# GET with Want-Digest: sha-256 — backend supplies → SDK emits
# Digest: sha-256=<hex>.
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Want-Digest: sha-256" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 200 " \
    && pass "GET with Want-Digest still returns 200" \
    || fail "GET Want-Digest status"
echo "$RESP" | grep -qi "^Digest: sha-256=" \
    && pass "GET emits Digest: sha-256= header" \
    || fail "GET missing Digest header"

# HEAD inherits the same Digest emission.
RESP=$(curl "${CURL_OPTS[@]}" -i -X HEAD -H "Want-Digest: sha-256" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 200 " \
    && pass "HEAD with Want-Digest returns 200" \
    || fail "HEAD Want-Digest status"
echo "$RESP" | grep -qi "^Digest: sha-256=" \
    && pass "HEAD inherits Digest emission" \
    || fail "HEAD missing Digest header"

# GET without Want-Digest — no Digest header emitted.
RESP=$(curl "${CURL_OPTS[@]}" -i "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
! echo "$RESP" | grep -qi "^Digest:" \
    && pass "GET without Want-Digest omits Digest header" \
    || fail "GET added unsolicited Digest"

# Want-Digest with unsupported algo — backend returns AXL_ERR, SDK
# omits the header rather than emitting bogus output.
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Want-Digest: md5" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
! echo "$RESP" | grep -qi "^Digest:" \
    && pass "GET Want-Digest unsupported-algo omits Digest" \
    || fail "GET emitted Digest despite backend AXL_ERR"

# Want-Digest with a totally unknown single algo — no header.
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Want-Digest: bogus-algo" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
! echo "$RESP" | grep -qi "^Digest:" \
    && pass "GET Want-Digest unknown-algo omits Digest" \
    || fail "GET emitted Digest for unknown algo"

# Want-Digest with multi-algo list — SDK should try each, find the
# one the backend supports, emit it (sha-256 here).
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Want-Digest: md5, sha-256" \
    "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | grep -qi "^Digest: sha-256=" \
    && pass "GET Want-Digest multi-algo picks the supported one" \
    || fail "GET multi-algo Want-Digest"

# Range request — Digest covers the FULL file (RFC 3230 §4.3.2),
# not the slice. Emission still happens on 206.
RESP=$(curl "${CURL_OPTS[@]}" -i -H "Want-Digest: sha-256" \
    -H "Range: bytes=0-5" "${BASE_URL}/dav/preset-stat" 2>/dev/null || true)
echo "$RESP" | head -1 | grep -q " 206 " \
    && pass "Range + Want-Digest returns 206" \
    || fail "Range + Want-Digest status"
echo "$RESP" | grep -qi "^Digest: sha-256=" \
    && pass "Range response still emits Digest (over full file)" \
    || fail "Range omitted Digest"

# --- WebDAV W7b: PUT Content-Digest validation (symmetric) ---
# Test backend returns a fixed 64-char hex for every file present
# in its in-memory FS. PUT round-trips that match the fixed hex
# pass; mismatches return 400 and the file is removed.

FIXED_HEX="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

# PUT with matching Content-Digest → 201 + file lands.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Digest: sha-256=${FIXED_HEX}" \
    -d "matching-digest-body" \
    "${BASE_URL}/dav/cd-good.tmp" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "PUT with matching Content-Digest returns 201" \
                       || fail "PUT Content-Digest match (got $CODE)"
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND -H "Depth: 0" "${BASE_URL}/dav/cd-good.tmp" 2>/dev/null || true)
[[ "$CODE" == "207" ]] && pass "PUT match left file on disk" \
                       || fail "PUT match (post-PROPFIND $CODE)"

# PUT with mismatched Content-Digest → 400 + file removed.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Digest: sha-256=deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef" \
    -d "mismatched-body" \
    "${BASE_URL}/dav/cd-bad.tmp" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "PUT with mismatched Content-Digest returns 400" \
                       || fail "PUT Content-Digest mismatch (got $CODE)"
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND -H "Depth: 0" "${BASE_URL}/dav/cd-bad.tmp" 2>/dev/null || true)
[[ "$CODE" == "404" ]] && pass "PUT mismatch removed corrupt file" \
                       || fail "PUT mismatch cleanup (post-PROPFIND $CODE)"

# PUT with NO Content-Digest → 201 (opt-out — validation only fires
# when the client asked for it).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -d "no-digest-body" \
    "${BASE_URL}/dav/cd-no.tmp" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "PUT without Content-Digest unaffected (201)" \
                       || fail "PUT no-digest (got $CODE)"

# PUT with unknown algo → 201 (SDK can't verify, passes through —
# documented contract: client's wishful request shouldn't break the
# upload when consumer can't supply that algo).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Digest: md5=deadbeefdeadbeefdeadbeefdeadbeef" \
    -d "unknown-algo-body" \
    "${BASE_URL}/dav/cd-unsupp.tmp" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "PUT with unsupported Content-Digest algo passes through (201)" \
                       || fail "PUT unsupported algo (got $CODE)"

# Empty-body PUT must validate too — an empty file has a known
# sha-256 (e3b0c44...) and we don't want a wrong-digest empty PUT
# to silently succeed. With a wrong claimed hex → 400 + cleanup.
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Length: 0" --data-binary "" \
    -H "Content-Digest: sha-256=deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef" \
    "${BASE_URL}/dav/cd-empty-bad.tmp" 2>/dev/null || true)
[[ "$CODE" == "400" ]] && pass "PUT empty body + mismatched Content-Digest returns 400" \
                       || fail "PUT empty + mismatched (got $CODE)"
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PROPFIND -H "Depth: 0" "${BASE_URL}/dav/cd-empty-bad.tmp" 2>/dev/null || true)
[[ "$CODE" == "404" ]] && pass "PUT empty mismatch removed file" \
                       || fail "PUT empty mismatch cleanup (post-PROPFIND $CODE)"

# Empty-body PUT with matching hex → 201 (validates against the
# test backend's FIXED_HEX, which the backend returns for every
# file in fs — empty or not).
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" \
    -X PUT -H "Content-Length: 0" --data-binary "" \
    -H "Content-Digest: sha-256=${FIXED_HEX}" \
    "${BASE_URL}/dav/cd-empty-good.tmp" 2>/dev/null || true)
[[ "$CODE" == "201" ]] && pass "PUT empty body + matching Content-Digest returns 201" \
                       || fail "PUT empty + matching (got $CODE)"

# Pipelined regression: middleware rejection on an upload route MUST
# force-close the connection. The client almost always sends headers
# and body in one write; if the server stays in keep-alive after the
# rejection, the leftover body bytes get parsed as the next request
# line and the next response is a confused 400 instead of EOF /
# RST. curl per-call hides this (each invocation opens a fresh
# connection); use a raw socket to keep one connection across two
# requests.
while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
    esac
done < <(python3 - "$HOST_PORT" << 'PYEOF'
import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(("127.0.0.1", port))
    # Note: NO Connection: close — we want the server to default to
    # keep-alive so we can probe the post-rejection state.
    s.sendall(
        b"POST /upload HTTP/1.1\r\n"
        b"Host: 127.0.0.1\r\n"
        b"X-Test-Reject: 1\r\n"
        b"Content-Length: 17\r\n"
        b"\r\n"
        b"should-not-arrive"
    )
    # Drain the 403 response (headers + body).
    first = b""
    s.settimeout(5)
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        first += chunk
        # Stop once we have the full Content-Length body. Cheap
        # heuristic: if we've read past header end + a few bytes,
        # call it done.
        if b"\r\n\r\n" in first and len(first) > first.index(b"\r\n\r\n") + 4 + 10:
            break
    if b" 403 " not in first.split(b"\r\n")[0]:
        print(f"FAIL: pipelined: first response not 403 ({first[:80]!r})")
        sys.exit(0)
    print("PASS: /upload pipelined: first response is 403")

    # Try to send a second request on the same socket. Server must
    # have closed: send may succeed (kernel buffer) but recv must
    # see EOF, NOT a confused-server response.
    try:
        s.sendall(
            b"GET /api/version HTTP/1.1\r\n"
            b"Host: 127.0.0.1\r\n"
            b"Connection: close\r\n"
            b"\r\n"
        )
    except (BrokenPipeError, ConnectionResetError):
        print("PASS: /upload pipelined: server closed (send raised, no desync)")
        sys.exit(0)

    second = b""
    s.settimeout(3)
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            second += chunk
    except (socket.timeout, ConnectionResetError):
        pass

    if not second:
        print("PASS: /upload pipelined: server closed (recv EOF, no desync)")
    elif b" 400 " in second.split(b"\r\n")[0]:
        print(f"FAIL: /upload pipelined: server desynced (parsed body as next request: {second[:80]!r})")
    else:
        # Some other response means server stayed alive and somehow
        # served the new GET — also wrong (we expected the connection
        # to be torn down).
        first_line = second.split(b"\r\n")[0].decode("ascii", errors="replace")
        print(f"FAIL: /upload pipelined: server stayed in keep-alive after rejection ({first_line!r})")
finally:
    s.close()
PYEOF
)

# ---------------------------------------------------------------------------
# Upload handler receives abort signal on TCP teardown mid-upload
# (regression: handlers leaked per-request state across connections,
# causing cross-request data corruption — see axl-webfs's PUT path).
#
# Promise a 1 MB body, send 5 bytes (smaller than the 1024 chunk
# size, so no chunk flush ever fires), then close. Abort is the
# only way the handler gets to know about this upload.
# ---------------------------------------------------------------------------

# Snapshot abort count before the aborted upload (fresh cache key).
RESP=$(http_get "/upload-status?seq=ab-before")
BEFORE_BODY=$(echo "$RESP" | sed '$d')
BEFORE_ABORTS=$(echo "$BEFORE_BODY" | sed -n 's/.*"aborts":\([0-9]*\).*/\1/p')

python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, struct
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", port))
s.sendall(
    b"POST /upload HTTP/1.1\r\n"
    b"Host: 127.0.0.1\r\n"
    b"Content-Length: 1048576\r\n"
    b"Connection: close\r\n"
    b"\r\n"
    b"hello"
)
# Linger 0 forces RST so the server sees the abort promptly (rather
# than a graceful FIN that might let it finish reading the 5 bytes
# and stall waiting for the rest).
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
             struct.pack('ii', 1, 0))
s.close()
PYEOF

# Poll /upload-status for up to 2 s waiting for the abort to fire.
# Fixed sleep was flaky on slow CI / aa64 TCG.
AFTER_ABORTS=""
EXPECTED=$((${BEFORE_ABORTS:-0} + 1))
for _i in $(seq 1 40); do
    RESP=$(http_get "/upload-status?seq=ab-after-$_i")
    AFTER_BODY=$(echo "$RESP" | sed '$d')
    AFTER_ABORTS=$(echo "$AFTER_BODY" | sed -n 's/.*"aborts":\([0-9]*\).*/\1/p')
    [[ -n "$AFTER_ABORTS" && "$AFTER_ABORTS" -ge "$EXPECTED" ]] && break
    sleep 0.05
done
# Exact-match: aborts must increment by exactly one (not more) so a
# spurious double-fire doesn't pass silently.
if [[ "$AFTER_ABORTS" == "$EXPECTED" ]]; then
    pass "/upload abort: handler notified once on disconnect (aborts $BEFORE_ABORTS -> $AFTER_ABORTS)"
else
    fail "/upload abort: expected aborts=$EXPECTED, got $AFTER_ABORTS (body: $AFTER_BODY)"
fi

# ---------------------------------------------------------------------------
# WebSocket tests
# ---------------------------------------------------------------------------

echo ""
echo "  --- WebSocket Tests ---"

# WebSocket echo test using Python
while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
    esac
done < <(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, hashlib, base64, os, time

port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(("127.0.0.1", port))
except Exception as e:
    print(f"FAIL: ws connect ({e})")
    sys.exit(1)

# Handshake
key = base64.b64encode(os.urandom(16)).decode()
req = (
    "GET /ws-echo HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
)
s.sendall(req.encode())

resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk

status_line = resp.split(b"\r\n")[0].decode()
if "101" not in status_line:
    print(f"FAIL: ws handshake (got {status_line})")
    s.close()
    sys.exit(0)
print("PASS: ws handshake returns 101")

# RFC 6455 §4.2.2: the 101 response MUST carry "Connection: Upgrade"
# (not keep-alive) and the correct reason phrase, or standard WS clients
# (websocket-client, every browser) reject the handshake with
# "Invalid WebSocket Header". Exact-string asserts so a regression can't
# slip back in behind a lenient substring match.
if status_line == "HTTP/1.1 101 Switching Protocols":
    print("PASS: ws 101 reason phrase is 'Switching Protocols'")
else:
    print(f"FAIL: ws 101 reason phrase (got {status_line!r})")

header_block = resp.split(b"\r\n\r\n", 1)[0].decode("latin-1")
conn_vals = [ln.split(":", 1)[1].strip().lower()
             for ln in header_block.split("\r\n")
             if ln.lower().startswith("connection:")]
if conn_vals == ["upgrade"]:
    print("PASS: ws 101 sends 'Connection: Upgrade'")
else:
    print(f"FAIL: ws 101 Connection header (got {conn_vals})")

# Verify accept key
accept_input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
expected_accept = base64.b64encode(hashlib.sha1(accept_input.encode()).digest()).decode()
if expected_accept.encode() in resp:
    print("PASS: ws accept key correct")
else:
    print("FAIL: ws accept key mismatch")

# Send a masked text frame
msg = b"hello-ws"
frame = bytearray()
frame.append(0x81)  # FIN + TEXT
frame.append(0x80 | len(msg))  # MASK bit + length
mask_key = os.urandom(4)
frame.extend(mask_key)
for i, b in enumerate(msg):
    frame.append(b ^ mask_key[i % 4])
s.sendall(frame)

# Read echo response
time.sleep(1)
try:
    data = s.recv(1024)
    if len(data) >= 2:
        opcode = data[0] & 0x0F
        plen = data[1] & 0x7F
        payload = data[2:2+plen]
        if opcode == 1 and payload == msg:
            print("PASS: ws echo received correct data")
        else:
            print(f"FAIL: ws echo wrong (opcode={opcode}, payload={payload!r})")
    else:
        print("FAIL: ws echo no data")
except socket.timeout:
    print("FAIL: ws echo timeout")

# Close
time.sleep(0.5)
close_frame = bytearray([0x88, 0x80]) + os.urandom(4)
try:
    s.sendall(close_frame)
except:
    pass
s.close()
PYEOF
)

# WebSocket per-connection API (add_websocket_ex): per-client send + auth.
while IFS= read -r line; do
    case "$line" in
        "PASS:"*) pass "${line#PASS: }" ;;
        "FAIL:"*) fail "${line#FAIL: }" ;;
    esac
done < <(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, base64, os, time

port = int(sys.argv[1])

def ws_open(path, auth=None):
    """Open a WS upgrade to path; return (status_code:int, sock-or-None)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    req = (f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n")
    if auth:
        req += f"Authorization: {auth}\r\n"
    req += "\r\n"
    s.sendall(req.encode())
    # Read the handshake response ONE BYTE at a time so we stop exactly at the
    # header terminator and never swallow a server frame that follows the 101
    # in the same TCP segment (e.g. a greet-on-connect banner) — otherwise the
    # follow-up recv races segment coalescing and intermittently times out.
    resp = b""
    while b"\r\n\r\n" not in resp:
        b1 = s.recv(1)
        if not b1:
            break
        resp += b1
    try:
        status = int(resp.split(b" ")[1])
    except Exception:
        status = 0
    return status, (s if status == 101 else (s.close() or None))

def ws_send_text(s, msg):
    b = msg.encode()
    frame = bytearray([0x81, 0x80 | len(b)])
    mask = os.urandom(4)
    frame += mask + bytes(c ^ mask[i % 4] for i, c in enumerate(b))
    s.sendall(frame)

def ws_recv_text(s):
    time.sleep(0.5)
    data = s.recv(1024)
    if len(data) < 2:
        return None
    plen = data[1] & 0x7F
    return data[2:2 + plen].decode(errors="replace")

# 1) Per-client echo via axl_ws_send (open endpoint).
try:
    st, s = ws_open("/ws-echo-ex")
    if st != 101:
        print(f"FAIL: ws-ex handshake (status {st})")
    else:
        print("PASS: ws-ex handshake 101")
        ws_send_text(s, "hello")
        r = ws_recv_text(s)
        if r == "ex:hello":
            print("PASS: ws-ex per-client send (got 'ex:hello')")
        else:
            print(f"FAIL: ws-ex per-client send (got {r!r})")
        s.close()
except Exception as e:
    print(f"FAIL: ws-ex ({e})")

# 2) Auth-gated upgrade: no credentials must be rejected (no 101).
try:
    st, s = ws_open("/ws-auth")
    if st == 401:
        print("PASS: ws-auth unauthorized upgrade rejected 401")
    else:
        print(f"FAIL: ws-auth unauth not rejected (status {st})")
        if s:
            s.close()
except Exception as e:
    print(f"FAIL: ws-auth unauth ({e})")

# 3a) Server-initiated close from inside a frame handler (axl_ws_conn_close):
#     the server must send a close frame and not hang / re-arm a reset conn.
try:
    st, s = ws_open("/ws-close")
    if st != 101:
        print(f"FAIL: ws-close handshake (status {st})")
    else:
        ws_send_text(s, "bye")
        time.sleep(0.5)
        data = s.recv(1024)
        # A clean server close is either a WS close frame (0x88) or EOF.
        if data == b"" or (len(data) >= 1 and (data[0] & 0x0F) == 0x8):
            print("PASS: ws-close server-initiated close from handler")
        else:
            print(f"FAIL: ws-close unexpected reply ({data!r})")
        s.close()
except Exception as e:
    print(f"FAIL: ws-close ({e})")

# 3) Auth-gated upgrade with a valid token: 101 + identity surfaced.
try:
    st, s = ws_open("/ws-auth", auth="Bearer test-token")
    if st != 101:
        print(f"FAIL: ws-auth authorized handshake (status {st})")
    else:
        print("PASS: ws-auth authorized upgrade 101")
        ws_send_text(s, "whoami")
        r = ws_recv_text(s)
        if r == "user:testuser":
            print("PASS: ws-auth identity surfaced (got 'user:testuser')")
        else:
            print(f"FAIL: ws-auth identity (got {r!r})")
        s.close()
except Exception as e:
    print(f"FAIL: ws-auth authorized ({e})")

# 3b) Close-from-connect: axl_ws_conn_close called inside AXL_WS_CONNECT (then
#     returning AXL_OK) must close cleanly and not wedge the server.
try:
    st, s = ws_open("/ws-connect-close")
    if st != 101:
        print(f"FAIL: ws-connect-close handshake (status {st})")
    else:
        time.sleep(0.5)
        try:
            data = s.recv(1024)
        except Exception:
            data = b""
        if data == b"" or (len(data) >= 1 and (data[0] & 0x0F) == 0x8):
            print("PASS: ws-connect-close closed cleanly from CONNECT")
        else:
            print(f"FAIL: ws-connect-close unexpected ({data!r})")
        s.close()
except Exception as e:
    print(f"FAIL: ws-connect-close ({e})")

# 4) Greet-on-connect: a banner sent from AXL_WS_CONNECT must arrive AFTER the
#    101 (valid only because CONNECT now fires post-handshake). Read without
#    sending anything first.
try:
    st, s = ws_open("/ws-greet")
    if st != 101:
        print(f"FAIL: ws-greet handshake (status {st})")
    else:
        r = ws_recv_text(s)
        if r == "hi":
            print("PASS: ws-greet banner from CONNECT (got 'hi')")
        else:
            print(f"FAIL: ws-greet banner (got {r!r})")
        s.close()
except Exception as e:
    print(f"FAIL: ws-greet ({e})")

# 5) Reject-on-connect: AXL_ERR from AXL_WS_CONNECT drops the connection. The
#    101 is sent, then the socket closes — a follow-up recv sees EOF.
try:
    st, s = ws_open("/ws-reject")
    if st != 101:
        print(f"FAIL: ws-reject handshake (status {st})")
    else:
        time.sleep(0.5)
        try:
            data = s.recv(1024)
        except Exception:
            data = b""
        if data == b"":
            print("PASS: ws-reject closed after CONNECT returned AXL_ERR")
        else:
            print(f"FAIL: ws-reject not closed (got {data!r})")
        s.close()
except Exception as e:
    print(f"FAIL: ws-reject ({e})")

# 6) Oversized-frame guard (WS wedge regression): the server, on an "OVERSIZE"
#    trigger, attempts a 600 KB axl_ws_send whose framed size exceeds the
#    per-connection outbound budget (512 KB). That send must be REJECTED at
#    axl_ws_send (asserted on the server serial log by the bash block below,
#    WS-OVERSIZE-RC:), never admitted and handed to the one-Transmit-in-flight
#    transport as one giant send (the wedge). Here we assert the client half:
#    the connection stays usable — after the oversized attempt a normal frame
#    still echoes back. We drain any large binary frame a pre-fix (escape-hatch)
#    server would have admitted before looking for the echo.
try:
    st, s = ws_open("/ws-echo-ex")
    if st != 101:
        print(f"FAIL: ws-oversize handshake (status {st})")
    else:
        s.settimeout(10)
        ws_send_text(s, "OVERSIZE")
        ws_send_text(s, "hello")
        buf = b""
        def _need(n):
            global buf
            while len(buf) < n:
                chunk = s.recv(65536)
                if not chunk:
                    raise EOFError
                buf += chunk
        def _read_frame():
            global buf
            _need(2)
            plen = buf[1] & 0x7F
            op = buf[0] & 0x0F
            hdr = 2
            if plen == 126:
                _need(4); plen = int.from_bytes(buf[2:4], "big"); hdr = 4
            elif plen == 127:
                _need(10); plen = int.from_bytes(buf[2:10], "big"); hdr = 10
            _need(hdr + plen)
            pl = buf[hdr:hdr + plen]
            buf = buf[hdr + plen:]
            return op, bytes(pl)
        echoed = False
        try:
            for _ in range(4):
                op, pl = _read_frame()
                if op == 0x1 and pl == b"ex:hello":
                    echoed = True
                    break
        except Exception:
            pass
        if echoed:
            print("PASS: ws-oversize server responsive after oversized-frame reject")
        else:
            print("FAIL: ws-oversize no echo after oversized frame (server wedged?)")
        s.close()
except Exception as e:
    print(f"FAIL: ws-oversize ({e})")

# 7) Multi-chunk transport round-trip (WS wedge fix, Part B): a 200 KB frame is
#    accepted (< 512 KB budget) but spans ~7 transport chunks (32 KB each), so
#    axl_tcp_send_async must chunk-chain it and still deliver every byte in
#    order. Send "BIGFRAME", read the whole binary frame, verify length + the
#    position-dependent pattern (byte i == i & 0xFF). Byte-exact receipt proves
#    the bounded-Transmit rewrite preserves correctness.
try:
    st, s = ws_open("/ws-echo-ex")
    if st != 101:
        print(f"FAIL: ws-bigframe handshake (status {st})")
    else:
        s.settimeout(15)
        ws_send_text(s, "BIGFRAME")
        rbuf = bytearray()
        def _rneed(n):
            while len(rbuf) < n:
                chunk = s.recv(65536)
                if not chunk:
                    raise EOFError
                rbuf.extend(chunk)
        _rneed(2)
        op = rbuf[0] & 0x0F
        plen = rbuf[1] & 0x7F
        hdr = 2
        if plen == 126:
            _rneed(4); plen = int.from_bytes(rbuf[2:4], "big"); hdr = 4
        elif plen == 127:
            _rneed(10); plen = int.from_bytes(rbuf[2:10], "big"); hdr = 10
        _rneed(hdr + plen)
        payload = bytes(rbuf[hdr:hdr + plen])
        want = 200 * 1024
        expect = bytes((i & 0xFF) for i in range(want))
        if op != 0x2:
            print(f"FAIL: ws-bigframe wrong opcode 0x{op:x}")
        elif len(payload) != want:
            print(f"FAIL: ws-bigframe length {len(payload)} != {want}")
        elif payload != expect:
            print("FAIL: ws-bigframe payload corrupted across transport chunks")
        else:
            print("PASS: ws-bigframe 200 KB frame byte-exact across transport chunks")
        s.close()
except Exception as e:
    print(f"FAIL: ws-bigframe ({e})")
PYEOF
)

# Oversized-frame guard — assert the REJECT on the server serial log. On the
# "OVERSIZE" trigger the server attempted a 600 KB axl_ws_send; its framed size
# exceeds the 512 KB outbound budget, so it must be rejected (negative rc) with
# an over-budget warning — NOT admitted (rc 0) and handed to the transport as
# one unbounded Transmit (the single-threaded-server wedge). Pre-fix escape
# hatch admitted it and printed WS-OVERSIZE-RC:0.
test_clean_log
OVR_RC=$(grep -oE 'WS-OVERSIZE-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 | sed 's/.*://')
if grep -q 'WS-OVERSIZE-OOM' "$TEST_CLEAN_LOG"; then
    fail "oversized WS probe could not allocate its 600 KB buffer (test-env OOM, not a fix regression)"
elif [[ -n "$OVR_RC" && "$OVR_RC" -lt 0 ]]; then
    pass "oversized WS frame rejected at axl_ws_send (rc=$OVR_RC)"
else
    fail "oversized WS frame not rejected (WS-OVERSIZE-RC='${OVR_RC:-<absent>}', want negative)"
fi
grep -q 'exceeds outbound budget' "$TEST_CLEAN_LOG" \
    && pass "oversized WS frame logged an over-budget warning" \
    || fail "oversized WS frame over-budget warning missing from serial log"

# Multi-chunk transport round-trip — the 200 KB BIGFRAME send must be ACCEPTED
# (rc 0) at axl_ws_send; the client-side check above proves it arrived
# byte-exact after the transport chunk-chained it (~7 bounded Transmits).
BIG_RC=$(grep -oE 'WS-BIGFRAME-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1 | sed 's/.*://')
if grep -q 'WS-BIGFRAME-OOM' "$TEST_CLEAN_LOG"; then
    fail "big-frame probe could not allocate its 200 KB buffer (test-env OOM)"
elif [[ "$BIG_RC" == "0" ]]; then
    pass "200 KB WS frame accepted by axl_ws_send (rc=0)"
else
    fail "200 KB WS frame not accepted (WS-BIGFRAME-RC='${BIG_RC:-<absent>}', want 0)"
fi

# ---------------------------------------------------------------------------
# UEFI HTTP Client tests (UEFI fetches from host Python server)
# ---------------------------------------------------------------------------

echo ""
echo "  --- UEFI HTTP Client Tests ---"

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

echo ""
echo "Results: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
