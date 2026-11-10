#!/bin/bash
# test-meta: arch=both needs= est=35 local-only=0 guest=standard
# test-http-throughput-driver-qemu.sh -- AxlTcp transmit pacing in DRIVER MODE.
#
# A resident service (AxlService, a DXE driver) runs its loop only inside a
# firmware timer tick (axl_loop_attach_driver). A tick that made one pass and
# returned left the pacing poll one pass per tick, so a 1 MiB GET took 6.4 s
# (x64) / 8.8 s (aa64) on a 50 ms tick -- the firmware's 10 ms-per-frame drain
# again. The pacer is a BUSY idle source, which extends each tick for up to the
# loop's driver budget (default 5 ms).
#
# AxlTestNet.efi serve-big-driver: plain HTTP on 8080 pumped by a 50 ms tick,
# nothing tuned. Checks:
#   1. three GET /big: 200, exactly 1048576 bytes, the fixture body's md5, and
#      under THROUGHPUT_BOUND_SECS each;
#   2. once traffic ends the guest's CPU is under CPU_BUDGET_CORES (0.07): a
#      busy idle source left behind would spend every tick's whole budget,
#      ~0.10 core above an idle guest's 0.02-0.03.
#
# Usage: ./test/integration/test-http-throughput-driver-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
BIG_SIZE=1048576
# Per arch, DEBUG. One pass per tick: 6.4 s (x64) / 6.4-8.8 s (aa64). With the
# 5 ms budget: 0.34-0.39 s (x64) / 1.0-1.1 s (aa64, TCG), and 1.2-2.2 s on aa64
# run alongside the 6-worker suite pool.
if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    THROUGHPUT_BOUND_SECS="4.0"
else
    THROUGHPUT_BOUND_SECS="2.0"
fi
# A busy idle source left behind makes EVERY 50 ms tick spend its 5 ms budget:
# about 0.10 core on top of an idle guest's 0.02-0.03. 0.07 sits between.
CPU_BUDGET_CORES="0.07"
SAMPLE_SECS=6

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# The startup commands live in test-http-throughput-driver-qemu.nsh
test_use_nsh

test_build_image

echo "=== AxlTcp pacing in driver mode, 50 ms tick ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" 8080
test_run_background
echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> 8080"
trap 'test_cleanup' EXIT

if ! test_wait_boot "READY" 90; then
    echo "FAIL: server did not reach READY within ${TEST_WAITED_S} seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 2

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
under() { awk -v v="$1" -v b="$2" 'BEGIN { print (v + 0 < b + 0) ? 1 : 0 }'; }

BODY="$TEST_TMPDIR/big.bin"
# The fixture's body: byte i = top byte of (i * 2654435761) mod 2^32.
EXPECTED_MD5=$(python3 -c 'import sys; sys.stdout.buffer.write(bytes(((i * 2654435761) & 0xffffffff) >> 24 for i in range(1 << 20)))' \
    | md5sum | cut -d' ' -f1)

for r in 1 2 3; do
    rm -f "$BODY"
    out=$(curl -s -H "Connection: close" --max-time 60 -o "$BODY" \
               -w "%{http_code} %{size_download} %{time_total}" \
               "http://127.0.0.1:${HOST_PORT}/big" 2>/dev/null || true)
    code="" size="" secs=""
    read -r code size secs <<< "$out" || true
    md5=$(md5sum "$BODY" 2>/dev/null | cut -d' ' -f1 || true)
    echo "  GET /big #$r: code=${code:-none} bytes=${size:-0} secs=${secs:-?}"
    if [[ "$code" == "200" && "$size" == "$BIG_SIZE" && "$md5" == "$EXPECTED_MD5" ]]; then
        pass "GET /big #$r: 200, $BIG_SIZE bytes, md5 $EXPECTED_MD5"
    else
        fail "GET /big #$r: want 200/$BIG_SIZE/$EXPECTED_MD5, got ${code:-none}/${size:-0}/${md5:-none}"
    fi
    if [[ -n "$secs" && $(under "$secs" "$THROUGHPUT_BOUND_SECS") == 1 ]]; then
        pass "GET /big #$r: ${secs}s < ${THROUGHPUT_BOUND_SECS}s in driver mode"
    else
        fail "GET /big #$r: ${secs:-?}s is not under ${THROUGHPUT_BOUND_SECS}s (driver-tick budget)"
    fi
done

HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
# The guest's QEMU: this test's own when booted; on a leased guest
# TEST_QEMU_PID is empty and the pool names the guest's (AXL_POOL_QEMU_PID).
GUEST_QEMU_PID="${TEST_QEMU_PID:-${AXL_POOL_QEMU_PID:-}}"
read_cpu() { awk '{n=index($0,") ");split(substr($0,n+2),f," ");print f[12]+f[13]}' \
    "/proc/${GUEST_QEMU_PID}/stat" 2>/dev/null || true; }
sleep 2
c0=$(read_cpu); sleep "$SAMPLE_SECS"; c1=$(read_cpu)
if [[ -n "$c0" && -n "$c1" ]]; then
    CORES=$(awk -v a="$c0" -v b="$c1" -v hz="$HZ" -v t="$SAMPLE_SECS" 'BEGIN { printf "%.3f", (b - a) / (hz * t) }')
else
    CORES="n/a"
fi
echo "  idle CPU after traffic: ${CORES} cores"
if [[ "$CORES" != "n/a" && $(under "$CORES" "$CPU_BUDGET_CORES") == 1 ]]; then
    pass "driver tick back to idle after traffic: ${CORES} < ${CPU_BUDGET_CORES} cores"
else
    fail "driver tick still busy with nothing pending: ${CORES} cores (budget ${CPU_BUDGET_CORES})"
fi

echo ""
printf "HTTP throughput (driver mode): %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    test_clean_log; echo ""; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
