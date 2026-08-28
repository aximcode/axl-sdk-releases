#!/bin/bash
# test-meta: arch=x64 needs= est=48 local-only=0
# Post-traffic server CPU regression test.
#
# Boots an HTTPS server driven by a RESIDENT driver-tick loop
# (axl_loop_attach_driver at TPL_CALLBACK) — the SoftBMC console-mirror /
# AxlService pattern — sends a burst of real HTTPS requests through it (full
# accept -> TLS handshake -> dispatch -> respond -> close path), and then
# measures the server's STEADY-STATE CPU once the traffic has drained.
#
# This guards the regression cold-idle can't catch: serving requests leaves
# the loop spinning — a source/timer left armed, a connection stuck, a poll
# tick that never disarms. A clean server returns to ~zero CPU after traffic;
# a leaky one keeps burning a core. (It also confirms the server is still
# responsive afterward — a post-traffic liveness GET.)
#
# CPU is sampled from /proc/<qemu-pid>/stat as host cores (utime+stime delta
# over the window; 1.0 = one host core saturated) — the same metric
# run-qemu.sh's --cpu-report prints. Built on the test-https-driver-qemu.sh
# harness (reliable connect -r + DHCP networking + host port-forward).
#
# X64-only (AARCH64/TCG has no NIC link).
# Usage: ./test/integration/test-server-cpu-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    echo "SKIP: server-CPU test is X64-only (AARCH64/TCG has no NIC link)"
    exit 0
fi

HOST_PORT=$(test_port 0)
GUEST_PORT=8443
REQUESTS=20
SETTLE_SECS=3        # let connections (TIME_WAIT) drain before sampling
SAMPLE_SECS=12       # post-traffic steady-state sampling window
# A drained server must sit FAR under one core. Post-traffic measures
# ~0.01-0.05 cores; a spin pegs ~1.0. 0.50 separates them with wide margin.
CPU_BUDGET_CORES="0.50"

TEST_BUILD_DIR="$(test_build_dir)"
make -C "$PROJECT_DIR" \
    ARCH=x64 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests echo-server-sync 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

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

echo Starting HTTPS server (resident driver-tick loop)...
AxlTestNet.efi serve-tls-driver
NSHEOF

test_build_image

echo "=== Post-traffic server CPU regression test ($TEST_ARCH) ==="
echo "    mode: serve-tls-driver (HTTPS, attach_driver pump — SoftBMC pattern)"

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_for "READY" 60; then
    echo "FAIL: HTTPS server did not start within 60 seconds"
    test_clean_log; echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server READY"
sleep 2

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

BASE_URL="https://127.0.0.1:${HOST_PORT}"
CURL=(curl -s --insecure -H "Connection: close" --max-time 8 -o /dev/null -w "%{http_code}")

# --- Send a burst of real HTTPS traffic through the full server path ---
echo "  Sending $REQUESTS HTTPS requests..."
served=0
for _ in $(seq 1 "$REQUESTS"); do
    code=$("${CURL[@]}" "${BASE_URL}/api/version" 2>/dev/null || echo 000)
    [[ "$code" == "200" ]] && served=$((served + 1))
done
[[ $served -eq $REQUESTS ]] \
    && pass "served $served/$REQUESTS HTTPS requests" \
    || fail "served only $served/$REQUESTS HTTPS requests (server path broken)"

# --- Measure CPU AFTER the traffic has drained ---
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
read_cpu() { awk '{n=index($0,") ");split(substr($0,n+2),f," ");print f[12]+f[13]}' \
    "/proc/${TEST_QEMU_PID}/stat" 2>/dev/null; }

echo "  Settling ${SETTLE_SECS}s, then sampling CPU for ${SAMPLE_SECS}s (post-traffic)..."
sleep "$SETTLE_SECS"
c0=$(read_cpu)
sleep "$SAMPLE_SECS"
c1=$(read_cpu)

if [[ -z "$c0" || -z "$c1" ]]; then
    fail "could not sample QEMU CPU (process gone?)"
    CORES="n/a"
else
    CORES=$(awk -v a="$c0" -v b="$c1" -v hz="$HZ" -v t="$SAMPLE_SECS" \
        'BEGIN{printf "%.3f", (b-a)/(hz*t)}')
    over=$(awk -v c="$CORES" -v b="$CPU_BUDGET_CORES" 'BEGIN{print (c+0>b+0)?"1":"0"}')
    [[ "$over" == "0" ]] \
        && pass "post-traffic CPU ${CORES} cores (budget ${CPU_BUDGET_CORES}) — server drained to idle" \
        || fail "post-traffic CPU ${CORES} cores > ${CPU_BUDGET_CORES} — server is SPINNING after serving"
fi

# --- Liveness: still responsive after the idle window (not wedged) ---
code=$("${CURL[@]}" "${BASE_URL}/api/version" 2>/dev/null || echo 000)
[[ "$code" == "200" ]] \
    && pass "server still responsive after the idle window (200)" \
    || fail "server unresponsive after idle (got '$code') — wedged after serving"

# Done with the harness server; stop it before the standalone run-qemu check
# so the two QEMUs don't contend.
kill "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0

# --- Exercise run-qemu.sh --cpu-report (the standalone CPU reporter) ---
# Asserts the output contract consumers grep ("CPU-REPORT: mean <m> cores,
# peak <p> cores") on a deterministically-idle server (echo-server-sync
# self-inits networking then blocks forever in a sync accept). This covers
# the --cpu-report sampler/print path the harness measurement above does not.
echo "  Checking run-qemu.sh --cpu-report output contract..."
RREPORT=$("$PROJECT_DIR/scripts/run-qemu.sh" --net --cpu-report --timeout 16 \
    "$TEST_BUILD_DIR/echo-server-sync.efi" 7000 2>&1 \
    | grep -oE 'CPU-REPORT: mean [0-9]+\.[0-9]+ cores, peak [0-9]+\.[0-9]+ cores' \
    | tail -1)
[[ -n "$RREPORT" ]] \
    && pass "run-qemu --cpu-report emits a well-formed line ($RREPORT)" \
    || fail "run-qemu --cpu-report did not emit a well-formed CPU-REPORT line"

echo ""
echo "  >> serve-tls-driver post-traffic steady-state CPU: ${CORES} cores ($(awk -v c="$CORES" 'BEGIN{printf "%.1f", (c+0)*100}')% of one core)"
printf "server-CPU tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; test_clean_log; echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
fi
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
