#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=0
# axl-kernel K6 integration test — runs axlk-hwinfo-server.efi as
# an axl-kernel process in QEMU, drives it with curl from the host
# via hostfwd, verifies each endpoint returns valid JSON. This is
# the go/no-go gate from AXL-Kernel-Design.md §9 K6: prove that a
# real-world service (HTTP + SMBIOS) fits naturally in the
# sequential-process model.
#
# Usage: ./test/integration/test-axlk-hwinfo.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all axlk-hwinfo-server 2>&1 | tail -3

TEST_BUILD_DIR="$(test_build_dir)"
test_add_efi "$TEST_BUILD_DIR/axlk-hwinfo-server.efi"

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

echo Starting axlk-hwinfo-server...
axlk-hwinfo-server.efi
NSHEOF

test_build_image

echo "=== axl-kernel K6 Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server..."

if ! test_wait_for "listening on port" 60; then
    echo "FAIL: axlk-hwinfo-server did not start within 60s"
    test_clean_log
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

sleep 2

CURL_OPTS=(-s -H "Connection: close" --max-time 10)
BASE="http://127.0.0.1:${HOST_PORT}"
FAIL=0

fetch() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "$BASE$1" 2>/dev/null || true
}

check_endpoint() {
    local name="$1"
    local path="$2"
    local needle="$3"
    local out code body
    out=$(fetch "$path")
    code=$(echo "$out" | tail -1)
    body=$(echo "$out" | head -n -1)

    if [[ "$code" != "200" ]]; then
        echo "  FAIL: $name got HTTP $code"
        FAIL=$((FAIL + 1))
        return
    fi
    if [[ "$body" != *"$needle"* ]]; then
        echo "  FAIL: $name body missing '$needle'"
        echo "    body: $body"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: $name (HTTP $code, contains '$needle')"
    echo "    body: $body"
}

check_endpoint "index"  "/"        "\"endpoints\""
check_endpoint "system" "/system"  "\"firmware\""
check_endpoint "cpu"    "/cpu"     "\"processor\""

# 404 test: a request to /nope should return 404 but still close cleanly.
NOT_FOUND_OUT=$(fetch "/nope")
NOT_FOUND_CODE=$(echo "$NOT_FOUND_OUT" | tail -1)
if [[ "$NOT_FOUND_CODE" == "404" ]]; then
    echo "  PASS: 404 on unknown path"
else
    echo "  FAIL: expected 404 for /nope, got $NOT_FOUND_CODE"
    FAIL=$((FAIL + 1))
fi

# One extra curl to bring total clients to MAX (5) so server exits.
fetch "/" >/dev/null

# Give the kernel a moment to print its PASS + clean exit.
sleep 3
kill "$TEST_QEMU_PID" 2>/dev/null || true
wait "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0

test_clean_log

PASS_COUNT=0
check_log() {
    local name="$1" pattern="$2"
    if grep -q "$pattern" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}
check_log "server-started"  "listening on port ${GUEST_PORT}"
check_log "served-clients"  "^PASS: axlk-hwinfo-server"
check_log "clean-exit"      "kernel exited rc=0"

echo ""
printf "axl-kernel K6: FAIL=%d\n" "$FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo "--- Serial log (tail) ---"
    tail -50 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
