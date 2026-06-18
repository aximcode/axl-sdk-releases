#!/bin/bash
# test-meta: arch=x64 needs= est=16 local-only=0
# echo-client integration test — boots QEMU with networking, runs the
# sync echo-client example against a host-side TCP echo server via
# the SLIRP gateway at 10.0.2.2, verifies the round-trip.
#
# Usage: ./test/integration/test-echo-client.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

ECHO_HOST_PORT=$(test_port 0)
ECHO_MESSAGE="hello from UEFI"

# Build library + the echo-client example
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all echo-client 2>&1 | tail -3

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/echo-client.efi"

# Startup: init network, run echo-client against host gateway
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    echo "echo Connecting drivers..."
    echo "connect -r"
    echo "stall 1000000"
    echo ""
    echo "echo Configuring network via DHCP..."
    echo "ifconfig -s eth0 dhcp"
    echo "stall 3000000"
    echo ""
    echo "echo Running echo-client test..."
    echo "echo-client.efi 10.0.2.2 ${ECHO_HOST_PORT} \"${ECHO_MESSAGE}\""
    echo ""
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== echo-client Integration Test ($TEST_ARCH) ==="

# Start host-side TCP echo server
ECHO_PID=0
python3 "$(dirname "$0")/tcp-echo-server.py" "$ECHO_HOST_PORT" &
ECHO_PID=$!

trap 'test_cleanup; [[ $ECHO_PID -gt 0 ]] && kill $ECHO_PID 2>/dev/null || true' EXIT

sleep 1
echo "  TCP echo server PID: $ECHO_PID, port: $ECHO_HOST_PORT"

# Boot QEMU with networking (SLIRP user-mode; outbound TCP goes via
# 10.0.2.2 automatically)
test_build_qemu_cmd
test_add_network
test_run_foreground 60

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

check "client-connected"    "^connected$"
check "client-sent"         "^sent: ${ECHO_MESSAGE}$"
check "client-received"     "^recv: ECHO:${ECHO_MESSAGE}$"

echo ""
printf "echo-client tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
