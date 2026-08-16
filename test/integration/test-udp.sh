#!/bin/bash
# test-meta: arch=x64 needs= est=13 local-only=0
# AxlUdp integration test — boots QEMU with networking, sends a UDP
# datagram to a host-side echo server, verifies the response.
#
# Usage: ./test/integration/test-udp.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

UDP_HOST_PORT=$(test_port 0)

# Build
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

TEST_BUILD_DIR="$(test_build_dir)"
test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Startup: init network, run UDP echo test to host gateway (10.0.2.2)
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
    echo "echo Running UDP echo test..."
    echo "AxlTestNet.efi udp-echo 10.0.2.2 ${UDP_HOST_PORT}"
    echo ""
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlUdp Integration Test ($TEST_ARCH) ==="

# Start host-side UDP echo server
ECHO_PID=0
python3 "$(dirname "$0")/udp-echo-server.py" "$UDP_HOST_PORT" &
ECHO_PID=$!

trap 'test_cleanup; [[ $ECHO_PID -gt 0 ]] && kill $ECHO_PID 2>/dev/null || true' EXIT

sleep 1
echo "  UDP echo server PID: $ECHO_PID, port: $UDP_HOST_PORT"

# Boot QEMU with networking (user-mode, no port forward needed for
# outbound UDP — QEMU's SLIRP forwards outbound UDP automatically)
test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device e1000,netdev=net0
    -netdev "user,id=net0"
)
test_run_foreground 45

test_clean_log

# Check results
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

check "udp-socket-opened"    "UDP-ECHO: sending"
check "udp-get-local-addr"   "PASS: udp-get-local-addr"
check "udp-echo-response"    "PASS: udp-echo-response"
check "udp-response-content" "ECHO:hello from UEFI"

echo ""
printf "UDP tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
