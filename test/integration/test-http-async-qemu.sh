#!/bin/bash
# test-meta: arch=x64 needs=openssl est=23 local-only=0 guest=standard
# test-http-async-qemu.sh — exercises the ASYNC HTTP client
# (axl_http_get_async / axl_http_post_async).
#
# The async request is driven entirely on a loop pumped by a resident
# driver tick at raised TPL (axl_loop_attach_driver) and is INITIATED from
# inside that raised-TPL tick — the SoftBMC alert-webhook topology. The sync
# axl_http_get/post would nest an ephemeral loop here and emit the
# "synchronous wait invoked from inside a loop callback" warning; the async
# path must complete with NO such warning. This test asserts both:
#   1. the response body arrives via the completion callback (GET and POST,
#      over both http and https), and
#   2. no synchronous-wait re-entrancy warning appears in the log.
#
# X64-only (SLIRP outbound; AARCH64/TCG has no NIC link). https needs openssl.
# Usage: ./test/integration/test-http-async-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1
test_parse_args "$@"
test_setup

if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    echo "SKIP: http-async test is X64-only (AARCH64/TCG has no NIC link)"
    exit 0
fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "SKIP: http-async test needs openssl (for the host https server cert)"
    exit 0
fi

PLAIN_PORT=$(test_port 0)   # plain-http server
TLS_PORT=$(test_port 1)     # https server

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 \
    all tests 2>&1 | tail -3
test_add_efi "$(test_build_dir x64)/AxlTestNet.efi"

CERT_DIR=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -keyout "$CERT_DIR/key.pem" \
    -out "$CERT_DIR/cert.pem" -days 1 -nodes -subj "/CN=10.0.2.2" \
    >/dev/null 2>&1

python3 "$(dirname "$0")/host-server.py" "$PLAIN_PORT" &
PLAIN_SERVER_PID=$!
python3 "$(dirname "$0")/host-server.py" "$TLS_PORT" \
    --tls "$CERT_DIR/cert.pem" "$CERT_DIR/key.pem" &
TLS_SERVER_PID=$!
trap 'test_cleanup; [[ $PLAIN_SERVER_PID -gt 0 ]] && kill $PLAIN_SERVER_PID 2>/dev/null; [[ $TLS_SERVER_PID -gt 0 ]] && kill $TLS_SERVER_PID 2>/dev/null; rm -rf "$CERT_DIR"' EXIT
sleep 1
echo "  Host plain server PID: $PLAIN_SERVER_PID, port: $PLAIN_PORT"
echo "  Host https server PID: $TLS_SERVER_PID, port: $TLS_PORT"

# The startup commands live in test-http-async-qemu.nsh; the plain server
# port is %1, the https one %2.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh "$PLAIN_PORT" "$TLS_PORT"

test_build_image

echo "=== async HTTP client Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_network
test_run_foreground 90

test_clean_log

PASS=0
FAIL=0

check_count() {  # <pattern> <min-count> <label>
    local n
    # grep -c exits 1 on zero matches; under `set -e` that would abort, so
    # swallow the status and keep the count.
    n=$(grep -c "$1" "$TEST_CLEAN_LOG" || true)
    if [[ "$n" -ge "$2" ]]; then
        echo "  PASS: $3 ($n)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $3 (got $n, want >= $2)"; FAIL=$((FAIL + 1))
    fi
}

# GET (http + https async) + the raised-TPL sync GET each return /hello.
check_count "hello from host" 3 "GET body received (async http+https, sync-rtpl)"
# The sync axl_http_get progressed at raised TPL (the Poll-tick path).
check_count "PASS: http-get-sync-rtpl" 1 "sync GET at raised TPL (poll-tick drives async core)"
# POST (http + https) each echo their distinct body back.
check_count "async-post-body" 1 "POST async http body echoed"
check_count "async-post-tls" 1 "POST async https body echoed"
# All four requests reported PASS from the completion callback.
check_count "PASS: http-async-GET" 2 "GET callbacks fired success (http + https)"
check_count "PASS: http-async-POST" 2 "POST callbacks fired success (http + https)"

# Large single-GET whole-body path (the v2.0.0 gBS->LoadImage-over-HTTP
# regression: the async core capped a Content-Length body at 1 MiB). A 1.5 MiB
# GET in one call must return the full, byte-exact body.
check_count "GET-SIZE-BYTES: 1572864" 2 "large single GET returns the full 1.5 MiB body (http + https)"
check_count "GET-SIZE-VERIFY: OK" 2 "large single GET body is byte-exact (http + https)"

# The headline of the whole effort: NO nested-loop synchronous-wait warning.
n_warn=$(grep -c "synchronous wait invoked from inside a loop callback" "$TEST_CLEAN_LOG" || true)
if [[ "$n_warn" -eq 0 ]]; then
    echo "  PASS: no synchronous-wait re-entrancy warning (async ran nest-free)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: $n_warn synchronous-wait warning(s) — async nested a loop"
    FAIL=$((FAIL + 1))
fi

# Pre-existing v2.0.0 leak (NOT the regression): a raised-TPL sync GET with a
# Connection: close response (every endpoint here closes) had req_drop_connection
# take the ASYNC close path on the ephemeral loop, which is freed before the
# close_event fires — leaking the socket + close-ctx and leaving a caller-owned
# loop source active. The get-sync-rtpl + get-size modes both run axl_http_get at
# TPL_CALLBACK, so a clean teardown must emit NEITHER the loop-free orphaned-source
# error NOR an AxlMem leak.
n_src=$(grep -c "caller-owned event source" "$TEST_CLEAN_LOG" || true)
if [[ "$n_src" -eq 0 ]]; then
    echo "  PASS: no orphaned caller-owned loop source (raised-TPL Connection: close close path)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: $n_src caller-owned event source leak(s) — raised-TPL close went async"
    FAIL=$((FAIL + 1))
fi
n_leak=$(grep -c "AxlMem leak" "$TEST_CLEAN_LOG" || true)
if [[ "$n_leak" -eq 0 ]]; then
    echo "  PASS: no AxlMem leak (raised-TPL Connection: close close path)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: $n_leak AxlMem leak(s) — raised-TPL sync GET leaked the socket/close-ctx"
    FAIL=$((FAIL + 1))
fi

echo ""
printf "http-async tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
