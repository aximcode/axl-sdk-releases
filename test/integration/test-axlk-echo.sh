#!/bin/bash
# axl-kernel K3 integration test — runs axlk-echo-server.efi inside
# QEMU (axl-kernel process spawning a handler per connection), fires
# three probes from the host via hostfwd, verifies each gets echoed.
#
# Validates: fd table, TCP syscalls, AxlLoop-integrated scheduler,
# fork-per-connection shape. The "it works like a real mini-OS" test.
#
# Usage: ./test/integration/test-axlk-echo.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=17000
GUEST_PORT=7000

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all axlk-echo-server 2>&1 | tail -3

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/axlk-echo-server.efi"

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

echo Starting axlk-echo-server...
axlk-echo-server.efi
NSHEOF

test_build_image

echo "=== axl-kernel K3 Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server..."

if ! test_wait_for "listening on port" 60; then
    echo "FAIL: axlk-echo-server did not start within 60s"
    test_clean_log
    echo "--- Serial log ---"
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

sleep 2

declare -a PROBE_RESULTS
MESSAGES=("hello-one" "hello-two" "hello-three")
FAIL=0
for i in 0 1 2; do
    msg="${MESSAGES[$i]}"
    out=$(python3 "$(dirname "$0")/tcp-probe.py" \
        127.0.0.1 "$HOST_PORT" "$msg" 5 2>&1 || true)
    PROBE_RESULTS[$i]="$out"
    if [[ "$out" == *"$msg"* ]]; then
        echo "  PASS: probe $i echoed '$msg'"
    else
        echo "  FAIL: probe $i expected '$msg' got '$out'"
        FAIL=$((FAIL + 1))
    fi
    sleep 1
done

# Give the kernel a moment to print its final PASS + clean exit.
sleep 3
kill "$TEST_QEMU_PID" 2>/dev/null || true
wait "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0

test_clean_log

PASS_COUNT=0
check_log() {
    local name="$1"
    local pattern="$2"
    if grep -q "$pattern" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $name (expected: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

check_log "server-listening"  "listening on port ${GUEST_PORT}"
check_log "accepted-clients"  "accepted client fd"
check_log "served-3-clients"  "^PASS: axlk-echo-server"
check_log "clean-kernel-exit" "kernel exited rc=0"

echo ""
printf "axl-kernel K3: %d passed (incl. 3 probes), %d failed\n" \
    $((PASS_COUNT + 3 - FAIL)) "$FAIL"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log (tail) ---"
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
