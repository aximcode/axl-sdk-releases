# shellcheck shell=bash
# http-test-lib.sh -- the boot and the helpers of the AxlNet HTTP tests:
# QEMU with port forwarding, the HTTP server started inside UEFI
# (test-http.nsh), validated with curl from the host.
#
# TWO TESTS, ONE BOOT SEQUENCE. This was test-http.sh, one boot in ~140 s;
# one boot is the unit the runner's pool parallelises, so it was split
# into two parts that source this file:
#
#   test-http.sh         the server: routes, auth, the cache, uploads,
#                        WebDAV, WebSocket
#   test-http-client.sh  the UEFI HTTP client against host-server.py
#                        (plain and TLS), which only this part starts
#                        (HTTP_TEST_HOST_SERVERS=1)
#
# Every check is in exactly one part. A part sources this file, which stages
# the guest; calls test_use_nsh itself; then http_boot, its cases and
# http_finish. A pooled test runs its OWN-named checked-in .nsh
# (check-test-meta holds every guest= test to one), so test-http-client.nsh
# is a copy of test-http.nsh -- and must stay one, which this file checks
# before either part boots.

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
if ! cmp -s "$(dirname "$0")/test-http.nsh" "$(dirname "$0")/test-http-client.nsh"; then
    echo "  FAIL: test-http-client.nsh is no longer a copy of test-http.nsh"
    exit 1
fi

# http_boot -- build the image, start the host servers the client part asks
# for (HTTP_TEST_HOST_SERVERS=1), boot, and wait for the server's READY.
http_boot() {
    test_build_image

    echo "=== AxlNet HTTP Integration Test: $HTTP_TEST_PART ($TEST_ARCH) ==="

    HOST_SERVER_PID=0
    HOST_TLS_PID=0
    CERT_DIR=""
    if [[ "${HTTP_TEST_HOST_SERVERS:-0}" == 1 ]]; then
        # ---------------------------------------------------------------------------
        # Start a Python HTTP server on the host for UEFI client tests.
        # The UEFI guest reaches it via QEMU gateway at 10.0.2.2.
        # ---------------------------------------------------------------------------

        HOST_SERVER_PORT=$(test_port 1)

        python3 "$(dirname "$0")/host-server.py" "$HOST_SERVER_PORT" &
        HOST_SERVER_PID=$!

        # The same server over TLS (a self-signed cert; the guest client sets
        # tls.verify=false), for how a close-framed body ends over TLS.
        HOST_TLS_PORT=$(test_port 2)
        CERT_DIR=$(mktemp -d)
        openssl req -x509 -newkey rsa:2048 -keyout "$CERT_DIR/key.pem" \
            -out "$CERT_DIR/cert.pem" -days 1 -nodes -subj "/CN=10.0.2.2" \
            >/dev/null 2>&1
        python3 "$(dirname "$0")/host-server.py" "$HOST_TLS_PORT" \
            --tls "$CERT_DIR/cert.pem" "$CERT_DIR/key.pem" &
        HOST_TLS_PID=$!

        sleep 1
        echo "  Host HTTP server PID: $HOST_SERVER_PID, port: $HOST_SERVER_PORT"
    fi
    trap 'test_cleanup; [[ $HOST_SERVER_PID -gt 0 ]] && kill $HOST_SERVER_PID 2>/dev/null; [[ $HOST_TLS_PID -gt 0 ]] && kill $HOST_TLS_PID 2>/dev/null; [[ -n "$CERT_DIR" ]] && rm -rf "$CERT_DIR"; true' EXIT

    # Boot QEMU with networking and port forwarding
    test_build_qemu_cmd
    test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
    test_run_background

    echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
    echo "  Waiting for HTTP server..."

    if ! test_wait_boot "READY" 60; then
        echo "FAIL: HTTP server did not start within ${TEST_WAITED_S} seconds"
        test_clean_log
        echo "--- Serial log ---"
        tail -20 "$TEST_CLEAN_LOG"
        echo "---"
        exit 1
    fi

    echo "  Server is ready"
    sleep 2
}

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

# http_finish -- the summary line and the exit status.
http_finish() {
    echo ""
    echo "Results: $PASS passed, $FAIL failed ($TEST_ARCH)"
    [[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
}
