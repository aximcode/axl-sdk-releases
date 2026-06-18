#!/bin/bash
# test-meta: arch=x64 needs= est=16 local-only=0
# Raised-TPL sync-TCP integration test — boots QEMU with networking and
# does a synchronous TCP connect + send + recv to a host-side echo server
# while at TPL_CALLBACK (the level a sync TCP op reaches when called from
# inside a driver-pump notify).
#
# This is the symmetric completion of the d249a9b6 raised-TPL wedge fix.
# That commit stopped the *wedge* (the nested loop no longer spins forever),
# but a sync TCP op still couldn't make *progress* at raised TPL: the sync
# connect/send/recv wrappers nested axl_loop_run with NO tcp4->Poll() tick,
# so the loop's CheckEvent spin held TPL_CALLBACK and starved the firmware
# TCP notify. The connect stalled to its full timeout and FAILED. The fix
# gives those wrappers a Poll() tick (armed only at raised TPL), exactly as
# the UDP sync wrapper does for UDP — so the connect advances.
#
# RED (pre-fix): connect stalls ~4 s then FAILs -> no "PASS: tcp-connect..".
# GREEN (fixed): connect + round-trip succeed.
#
# Usage: ./test/integration/test-tcp-connect-rtpl-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

# Outbound networking needs a working NIC link. Under AARCH64/TCG the
# virt machine brings up no usable NIC ("no link detected on any NIC"),
# so DHCP never gets a lease and any outbound connect fails regardless of
# the code under test. The sibling outbound-net tests (test-udp.sh,
# test-echo-client.sh, test-tcp-echo.sh) are X64-only for the same reason.
# The fix itself is arch-independent C and is covered cross-arch by the
# unit suite; this end-to-end check runs on X64 (KVM SLIRP).
if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    echo "SKIP: raised-TPL TCP test is X64-only (AARCH64/TCG has no NIC link)"
    exit 0
fi

ECHO_HOST_PORT=$(test_port 0)

# Build library + test binaries
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Startup: init network, run the raised-TPL TCP connect against the host
# gateway (10.0.2.2 — SLIRP forwards outbound TCP automatically).
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
    echo "echo Running raised-TPL TCP connect test..."
    echo "AxlTestNet.efi tcp-connect-rtpl 10.0.2.2 ${ECHO_HOST_PORT}"
    echo ""
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Raised-TPL sync-TCP Integration Test ($TEST_ARCH) ==="

# Start host-side TCP echo server (replies "ECHO:<msg>").
ECHO_PID=0
python3 "$(dirname "$0")/tcp-echo-server.py" "$ECHO_HOST_PORT" &
ECHO_PID=$!

trap 'test_cleanup; [[ $ECHO_PID -gt 0 ]] && kill $ECHO_PID 2>/dev/null || true' EXIT

sleep 1
echo "  TCP echo server PID: $ECHO_PID, port: $ECHO_HOST_PORT"

# Boot QEMU with networking (SLIRP user-mode; outbound TCP to 10.0.2.2
# reaches the host automatically).
test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device e1000,netdev=net0
    -netdev "user,id=net0"
)
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

# Headline: the connect advanced at TPL_CALLBACK (the bug under test).
check "tcp-connect-raised-tpl"   "PASS: tcp-connect-raised-tpl"
# Bonus: send + recv at raised TPL also advanced; server echoed.
check "tcp-roundtrip-raised-tpl" "PASS: tcp-roundtrip-raised-tpl"
check "tcp-echo-content"         "ECHO:hello from UEFI"

echo ""
printf "raised-TPL TCP tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
