#!/bin/bash
# test-meta: arch=x64 needs= est=19 local-only=0
# axl-kernel second SoftBMC-port test — runs axlk-bootconfig-server
# inside QEMU, drives it with curl from the host, verifies that it
# successfully reads UEFI NVRAM variables (BootOrder, Boot####,
# SecureBoot) and serves them as JSON.
#
# Purpose: a second-module pressure test beyond the HwInfo port,
# showing the sequential-process shape still holds when reading
# binary UEFI variable formats.
#
# Usage: ./test/integration/test-axlk-bootconfig.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8081

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all axlk-bootconfig-server 2>&1 | tail -3

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/axlk-bootconfig-server.efi"

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

echo Starting axlk-bootconfig-server...
axlk-bootconfig-server.efi
NSHEOF

test_build_image

echo "=== axl-kernel BootConfig Port ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server..."

if ! test_wait_for "listening on port" 60; then
    echo "FAIL: axlk-bootconfig-server did not start within 60s"
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
    local name="$1" path="$2" needle="$3"
    local out code body
    out=$(fetch "$path")
    code=$(echo "$out" | tail -1)
    body=$(echo "$out" | head -n -1)
    if [[ "$code" != "200" ]]; then
        echo "  FAIL: $name HTTP $code"
        FAIL=$((FAIL + 1))
        return
    fi
    if [[ "$body" != *"$needle"* ]]; then
        echo "  FAIL: $name body missing '$needle'"
        echo "    body: $body"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: $name (contains '$needle')"
    echo "    body: $body"
}

check_endpoint "overview"   "/"           "\"boot_order\""
check_endpoint "entries"    "/entries"    "\"entries\""
check_endpoint "secureboot" "/secureboot" "\"secure_boot\""

# 404 + filler to reach MAX_CLIENTS so server exits cleanly.
fetch "/nope"  >/dev/null
fetch "/"      >/dev/null

sleep 3
kill "$TEST_QEMU_PID" 2>/dev/null || true
wait "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0

test_clean_log

check_log() {
    local name="$1" pattern="$2"
    if grep -q "$pattern" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $name"
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}
check_log "server-started"  "listening on port ${GUEST_PORT}"
check_log "served-clients"  "^PASS: axlk-bootconfig-server"
check_log "clean-exit"      "kernel exited rc=0"

echo ""
printf "axl-kernel BootConfig: FAIL=%d (%s)\n" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- Serial log (tail) ---"
    tail -50 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
