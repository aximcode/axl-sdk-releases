#!/bin/bash
# test-sntp-qemu.sh — axl_sntp_query against a host-side mock SNTP server.
#
# Boots QEMU with SLIRP networking; the guest queries 10.0.2.2:<port> (SLIRP
# forwards guest outbound UDP to the host), where a mock SNTP responder serves
# a FIXED, known Unix time. The guest's axl_sntp_query must parse that exact
# value, proving the SNTP request/response round-trip + timestamp decode.
#
# Usage: ./test/integration/test-sntp-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

SNTP_HOST_PORT=19123
SNTP_UNIX_SECS=1700000000   # fixed, known time the mock serves

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

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
    echo "echo Querying SNTP..."
    echo "AxlTestNet.efi sntp-query 10.0.2.2 ${SNTP_HOST_PORT}"
    echo ""
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlSntp Integration Test ($TEST_ARCH) ==="

# Start the host-side mock SNTP responder.
SNTP_PID=0
python3 "$(dirname "$0")/sntp-server.py" "$SNTP_HOST_PORT" "$SNTP_UNIX_SECS" &
SNTP_PID=$!
trap 'test_cleanup; [[ $SNTP_PID -gt 0 ]] && kill $SNTP_PID 2>/dev/null || true' EXIT
sleep 1
echo "  SNTP responder PID: $SNTP_PID, port: $SNTP_HOST_PORT, unix_secs: $SNTP_UNIX_SECS"

# Boot with SLIRP networking (outbound UDP to 10.0.2.2 reaches the host).
# Use the arch-appropriate NIC: e1000 (x64) / virtio-net-pci (aa64, which AAVMF
# auto-binds; e1000 has no driver in the aa64 firmware).
test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device "$(_test_nic_device),netdev=net0"
    -netdev "user,id=net0"
)
test_run_foreground 45

test_clean_log

PASS=0
FAIL=0
check() {
    if grep -q "$2" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (expected: $2)"; FAIL=$((FAIL + 1))
    fi
}

check "sntp-reachable"      "PASS: sntp-reachable"
check "sntp-time-matches"   "PASS: sntp-unix-secs=${SNTP_UNIX_SECS}"

echo ""
printf "SNTP tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; cat "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
