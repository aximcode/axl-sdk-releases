#!/bin/bash
# test-meta: arch=x64 needs= est=18 local-only=0
# echo-server-sync integration test — runs the single-client sync
# echo server inside UEFI, connects from the host via hostfwd, and
# verifies one echo round-trip (guest-side serial log + host-side
# nc output).
#
# Usage: ./test/integration/test-echo-server-sync.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=7001
ECHO_MESSAGE="hello-from-host"

# Build library + the sync server example
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all echo-server-sync 2>&1 | tail -3

TEST_BUILD_DIR="$(test_build_dir)"
test_add_efi "$TEST_BUILD_DIR/echo-server-sync.efi"

# Startup: init network, launch server
cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting echo-server-sync...
echo-server-sync.efi ${GUEST_PORT}
NSHEOF

test_build_image

echo "=== echo-server-sync Integration Test ($TEST_ARCH) ==="

# Boot QEMU in background with hostfwd
test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server to start..."

if ! test_wait_for "single-client" 60; then
    echo "FAIL: echo-server-sync did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"
    tail -30 "$TEST_CLEAN_LOG"
    echo "---"
    exit 1
fi

echo "  Server is ready"
sleep 2

# Probe the server. Uses a small Python helper rather than nc so we
# don't depend on which nc variant is installed (openbsd-nc has -q,
# nmap-ncat does not).
PROBE_OUTPUT=$(python3 "$(dirname "$0")/tcp-probe.py" \
    127.0.0.1 "$HOST_PORT" "$ECHO_MESSAGE" 5 2>&1 || true)

# Give the guest a moment to log the "disconnected" line, then stop.
sleep 2
kill "$TEST_QEMU_PID" 2>/dev/null || true
wait "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0

test_clean_log

PASS=0
FAIL=0

check() {
    local name="$1"
    local pattern="$2"
    if grep -q "$pattern" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

check "server-listening"      "listening on port ${GUEST_PORT}"
check "server-saw-connect"    "^  connected$"
check "server-received-data"  "recv: ${ECHO_MESSAGE}"
check "server-saw-disconnect" "^  disconnected$"

if [[ "$PROBE_OUTPUT" == *"$ECHO_MESSAGE"* ]]; then
    echo "  PASS: host-received-echo"
    PASS=$((PASS + 1))
else
    echo "  FAIL: host-received-echo (probe output: '$PROBE_OUTPUT')"
    FAIL=$((FAIL + 1))
fi

echo ""
printf "echo-server-sync tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
