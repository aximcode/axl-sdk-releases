#!/bin/bash
# test-meta: arch=x64 needs= est=19 local-only=0
# test-webdav-fs-tls-qemu.sh — axl_http_server_serve_fs over TLS, keep-alive.
#
# The plain-HTTP test-webdav-fs-qemu.sh exercises curl's `-T -` upload (chunked
# + Expect: 100-continue) but only over http://, where a single-record body
# masks the TLS multi-record buffering bug. This boots the SAME serve_fs mount
# over HTTPS and drives the deadlock case directly: a keep-alive chunked PUT
# with Expect: 100-continue (what `curl -T -` sends by default) must complete
# (curl rc 0), not hang (rc 28). Regression guard for the TLS upload-path drain.
#
# Requires: AXL_TLS=1 build. Auxiliary single-binary test (opt out of ratchet).
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
make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" AXL_TLS=1 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

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
echo Starting WebDAV-over-TLS file server...
AxlTestNet.efi serve-davfs-tls
NSHEOF

test_build_image

echo "=== AxlNet serve_fs WebDAV over TLS Test ($TEST_ARCH) ==="

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

BASE="https://127.0.0.1:${HOST_PORT}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# ka_code <curl args...> ; echo "<rc> <http_code>". Captures curl's exit code
# (28 on a keep-alive hang) WITHOUT letting set -e abort the script.
ka_code() {
    local out rc
    out=$(curl -sk --max-time 8 -o /dev/null -w '%{http_code}' "$@" 2>/dev/null) \
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
b=$(curl -sk --max-time 8 "${BASE}/dav/chunked.txt" 2>/dev/null || true)
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
gz=$( { curl -sk --max-time 12 "${BASE}/dav/big-chunked.bin" 2>/dev/null || true; } | wc -c)
[[ "$gz" == 32768 ]] && pass "chunked-uploaded file is 32768 bytes (no chunk truncation)" \
    || fail "chunked-uploaded file truncated ($gz bytes, want 32768)"

# 4c) whole-body route, Content-Length.
read -r rc c < <(ka_code -X POST --data-binary @"$BIG" -H "Expect:" "${BASE}/echo")
[[ "$rc" == 0 && "$c" == 200 ]] && pass "TLS large POST /echo (Content-Length, 32K) -> $c" \
    || fail "TLS large POST /echo (CL) dropped (rc=$rc, code=$c)"

# 4d) whole-body route, chunked + verify the bytes round-trip intact.
sz=$( { curl -sk --max-time 12 -X POST -H "Transfer-Encoding: chunked" -H "Expect:" \
        --data-binary @"$BIG" "${BASE}/echo" 2>/dev/null || true; } | wc -c)
[[ "$sz" == 32768 ]] && pass "TLS large POST /echo (chunked) echoes 32768 bytes" \
    || fail "TLS large POST /echo (chunked) wrong echo size ($sz, want 32768)"
rm -f "$BIG"

echo ""
echo "  serve_fs WebDAV over TLS: $PASS passed, $FAIL failed ($TEST_ARCH)"
if [[ "$FAIL" -ne 0 ]]; then
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
fi
[[ "$FAIL" -eq 0 && "$PASS" -gt 0 ]] && exit 0 || exit 1
