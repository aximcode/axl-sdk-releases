#!/bin/bash
# rfbrowse.efi integration test — boots QEMU, runs rfbrowse against a
# Python mock Redfish server on the host, validates serial log output.
#
# Usage: ./test/integration/test-redfish.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

MOCK_PORT=18082

# Determine build output directory
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

# Stage rfbrowse.efi
test_add_efi "$TEST_BUILD_DIR/tools/rfbrowse.efi"

# Startup script: init network, run rfbrowse tests
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

echo [TEST START]

echo [test-root]
rfbrowse.efi http://10.0.2.2:${MOCK_PORT} --raw

echo [test-system]
rfbrowse.efi http://10.0.2.2:${MOCK_PORT} -u admin -p password --raw system

echo [test-thermal]
rfbrowse.efi http://10.0.2.2:${MOCK_PORT} -u admin -p password --raw thermal

echo [test-members]
rfbrowse.efi http://10.0.2.2:${MOCK_PORT} -u admin -p password --members systems

echo [test-noauth]
rfbrowse.efi http://10.0.2.2:${MOCK_PORT} --raw systems

echo [test-badpass]
rfbrowse.efi http://10.0.2.2:${MOCK_PORT} -u admin -p wrong system

echo [TEST DONE]
NSHEOF

test_build_image

echo "=== rfbrowse Redfish Integration Test ($TEST_ARCH) ==="

# Start mock Redfish server on host
python3 "$(dirname "$0")/redfish-mock-server.py" "$MOCK_PORT" &
MOCK_PID=$!

trap 'test_cleanup; kill $MOCK_PID 2>/dev/null || true' EXIT

sleep 1
echo "  Mock Redfish server PID: $MOCK_PID, port: $MOCK_PORT"

# Boot QEMU with user-mode networking (guest connects out to host)
test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device e1000,netdev=net0
    -netdev "user,id=net0"
)

test_run_foreground 60

echo "  QEMU finished, checking serial log..."

# ---------------------------------------------------------------------------
# Validate serial log
# ---------------------------------------------------------------------------

test_clean_log

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Check that a marker section contains expected text
check_section() {
    local section="$1"
    local pattern="$2"
    local desc="$3"
    local start_marker="\[${section}\]"

    if ! grep -q "$start_marker" "$TEST_CLEAN_LOG"; then
        fail "$desc (section '$section' not found in log)"
        return
    fi

    # Extract section content (between this marker and the next marker or end)
    if sed -n "/$start_marker/,/^\[test-\|^\[TEST /p" "$TEST_CLEAN_LOG" | grep -qF -- "$pattern"; then
        pass "$desc"
    else
        fail "$desc (pattern '$pattern' not found in section '$section')"
    fi
}

echo ""
echo "  --- rfbrowse Tests ---"

# Test 1: Service root (no auth) — should show RedfishVersion
check_section "test-root" "RedfishVersion" "Service root shows RedfishVersion"
check_section "test-root" "@odata.id" "Service root has @odata.id"

# Test 2: System resource (authenticated) — should show model/serial
check_section "test-system" "AximCode" "System shows Manufacturer"
check_section "test-system" "AXL-TEST-001" "System shows SerialNumber"
check_section "test-system" "QEMU Virtual Machine" "System shows Model"

# Test 3: Thermal resource — should show temperature readings
check_section "test-thermal" "CPU Temp" "Thermal shows CPU Temp"
check_section "test-thermal" "Inlet Temp" "Thermal shows Inlet Temp"
check_section "test-thermal" "42" "Thermal shows temperature reading"

# Test 4: Members listing
check_section "test-members" "/redfish/v1/Systems/1" "Members lists system URI"
check_section "test-members" "1 member" "Members shows count"

# Test 5: No auth on protected endpoint — should fail with 401
check_section "test-noauth" "401" "No-auth GET returns 401"

# Test 6: Bad password — should fail
check_section "test-badpass" "401" "Bad password rejected"

echo ""
echo "Results: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
