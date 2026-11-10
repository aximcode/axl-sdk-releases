#!/bin/bash
# test-meta: arch=x64 needs=openssl est=15 local-only=0 guest=standard
# test-https-client-qemu.sh — exercises the HTTP CLIENT over https.
#
# This is the one TLS path the serve-tls suite doesn't cover: axl_tls_connect
# (the client-side handshake), reached only by an outbound https request.
# After the strippable-TLS refactor (src/net/axl-http-client-tls.h), the client
# reaches TLS only through ops registered by axl_tls_init(); fetch calls that
# for https URLs. This test boots fetch.efi and GETs a host-side
# https server, confirming the client https path still works end to end.
#
# X64-only (SLIRP outbound; AARCH64/TCG has no NIC link). Requires openssl.
# Usage: ./test/integration/test-https-client-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1
test_parse_args "$@"
test_setup

if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    echo "SKIP: https-client test is X64-only (AARCH64/TCG has no NIC link)"
    exit 0
fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "SKIP: https-client test needs openssl (for the host server cert)"
    exit 0
fi

HOST_PORT=$(test_port 0)    # https server
PLAIN_PORT=$(test_port 1)   # plain-http redirector -> the https server (cross-scheme)

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 \
    all tools 2>&1 | tail -3
TOOLS_DIR="$(test_build_dir x64)/tools"
test_add_efi "$TOOLS_DIR/fetch.efi"

# Self-signed cert for the host https server. fetch defaults to insecure
# (tls.verify=false), so a self-signed cert is accepted — the point is the
# handshake + record layer, not chain validation.
CERT_DIR=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -keyout "$CERT_DIR/key.pem" \
    -out "$CERT_DIR/cert.pem" -days 1 -nodes -subj "/CN=10.0.2.2" \
    >/dev/null 2>&1

python3 "$(dirname "$0")/host-server.py" "$HOST_PORT" \
    --tls "$CERT_DIR/cert.pem" "$CERT_DIR/key.pem" &
HOST_SERVER_PID=$!
# Plain-http server whose /redirect 302s to https://.../hello — drives the
# http->https redirect path (fetch starts on http, so it never pre-inits TLS
# from the initial scheme; the redirect must still complete).
python3 "$(dirname "$0")/host-server.py" "$PLAIN_PORT" \
    --redirect-base "https://10.0.2.2:$HOST_PORT" &
PLAIN_SERVER_PID=$!
trap 'test_cleanup; [[ $HOST_SERVER_PID -gt 0 ]] && kill $HOST_SERVER_PID 2>/dev/null; [[ $PLAIN_SERVER_PID -gt 0 ]] && kill $PLAIN_SERVER_PID 2>/dev/null; rm -rf "$CERT_DIR"' EXIT
sleep 1
echo "  Host HTTPS server PID: $HOST_SERVER_PID, port: $HOST_PORT"
echo "  Host plain redirector PID: $PLAIN_SERVER_PID, port: $PLAIN_PORT"

# The startup commands live in test-https-client-qemu.nsh; the https port is
# %1, the plain redirector's %2.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh "$HOST_PORT" "$PLAIN_PORT"

test_build_image

echo "=== https client Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_network
test_run_foreground 60

test_clean_log

PASS=0
FAIL=0

# The handshake completed AND plaintext was decrypted iff the body arrives.
# Two GETs return the /hello body: the direct https GET and the http->https
# redirected GET. Both succeeding => "hello from host" appears twice. A
# regression where the redirect can't reach https (the tool only inits TLS for
# an https INITIAL scheme) drops the count to 1.
n_hello=$(grep -c "hello from host" "$TEST_CLEAN_LOG")
if [[ "$n_hello" -ge 1 ]]; then
    echo "  PASS: direct https GET body received (TLS handshake + decrypt)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: direct https GET body not received (expected: hello from host)"
    FAIL=$((FAIL + 1))
fi
if [[ "$n_hello" -ge 2 ]]; then
    echo "  PASS: http->https redirect followed (body received via redirected https GET)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: http->https redirect did not complete ($n_hello/2 'hello from host' bodies)"
    FAIL=$((FAIL + 1))
fi

# A body framed by the close ends in order over TLS only at close_notify:
# with it, the body arrives whole; with a bare TCP close instead (a cut,
# RFC 8446 6.1), the request fails and no part of the body is shown.
sec() { awk -v a="=== $1 ===" -v b="=== $2 ===" '$0 ~ a {on=1; next} $0 ~ b {on=0} on' "$TEST_CLEAN_LOG"; }
notify=$(sec TEST-TLS-CLOSE-NOTIFY TEST-TLS-CLOSE-CUT)
cut=$(sec TEST-TLS-CLOSE-CUT TEST-END)
if grep -qx "notify-body-intact" <<< "$notify" \
   && grep -qx "Fetch: request failed" <<< "$cut" \
   && ! grep -q "cut-body-no-notify" <<< "$cut"; then
    echo "  PASS: TLS close-framed body: whole at close_notify; a close without it fails the request"
    PASS=$((PASS + 1))
else
    echo "  FAIL: TLS close-framed body (close_notify section: '$notify'; bare close section: '$cut')"
    FAIL=$((FAIL + 1))
fi

echo ""
printf "https-client tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
