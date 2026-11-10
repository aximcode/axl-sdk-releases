#!/bin/bash
# test-meta: arch=x64 needs= est=17 local-only=0 guest=standard
# test-http-multi-qemu.sh — two axl_http_server instances on ONE AxlLoop.
#
# Regression for the bug where a second server on a shared loop silently
# stopped dispatching after the first (TLS) server handled a connection:
# the sync TLS-handshake recv left a stale recv_cancel_source id from its
# freed ephemeral loop, and axl_tcp_close later removed that id from the
# shared loop — deleting the second server's accept source (same id). The
# second server then bound + accepted TCP but never produced a response.
# This is SoftBMC's HTTPS:443 + HTTP:80-redirect shape.
#
# Boots AxlTestNet.efi serve-multi-tls (TLS server on 8443 started first,
# plain server on 8081 second, one loop), forwards both ports, curls the
# TLS server (triggers the handshake + close), THEN curls the plain
# server — which must still answer.
#
# Auxiliary single-binary test (opt out of the
# test-axl.sh ratchet).
#
# Usage: ./test/integration/test-http-multi-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

test_parse_args "$@"
test_setup

H_TLS=$(test_port 0)    # host -> guest 8443 (TLS server, started first)
H_PLAIN=$(test_port 1)  # host -> guest 8081 (plain server, started second)

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all tests 2>&1 | tail -2

test_add_efi "$(test_build_dir "$_native_arch")/AxlTestNet.efi"

# The startup commands live in test-http-multi-qemu.nsh
test_use_nsh

test_build_image

echo "=== AxlNet multi-server (TLS + plain on one loop) Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$H_TLS" 8443 "$H_PLAIN" 8081
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID  (host $H_TLS->8443 TLS, $H_PLAIN->8081 plain)"
echo "  Waiting for servers..."
if ! test_wait_boot "READY" 60; then
    echo "FAIL: servers did not start within ${TEST_WAITED_S} seconds"
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Servers ready"
sleep 2

CURL_OPTS=(-s -H "Connection: close" --max-time 10)
fail=0

# 1) Hit the TLS server first — this runs the blocking handshake and a
#    connection close, which is what used to orphan the plain server's
#    accept source.
rt=$(test_curl -k "${CURL_OPTS[@]}" -w '|%{http_code}' "https://127.0.0.1:${H_TLS}/plain" || true)
bt=${rt%|*}; ct=${rt##*|}
if [[ "$ct" == "200" && "$bt" == *"Hello from AxlNet"* ]]; then
    echo "  PASS: TLS server (8443) responded 200"
else
    echo "  FAIL: TLS server (8443) code='$ct' body='$bt'"; fail=1
fi

sleep 1

# 2) Now the plain second server must STILL dispatch.
rp=$(test_curl "${CURL_OPTS[@]}" -w '|%{http_code}' "http://127.0.0.1:${H_PLAIN}/plain" || true)
bp=${rp%|*}; cp=${rp##*|}
if [[ "$cp" == "200" && "$bp" == *"Hello from server2"* ]]; then
    echo "  PASS: plain server (8081) still dispatches after the TLS connection"
else
    echo "  FAIL: plain server (8081) code='$cp' body='$bp' (the multi-server bug)"; fail=1
fi

if (( fail )); then
    echo "FAIL: multi-server dispatch"
    exit 1
fi
echo "All multi-server checks passed."
exit 0
