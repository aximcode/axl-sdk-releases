#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-http-rebind-multi-qemu.sh — deferred-close finalize must be SCOPED.
#
# The multi-server-on-one-loop topology (SoftBMC's real deployment). TWO HTTP
# servers, A (:8080) and B (:8081), share one loop and both accumulate in-flight
# loop-deferred graceful closes. Then ONLY server A is abortive-freed + rebound
# with no pump. The abortive teardown finalizes A's deferred closes scoped by A's
# listener_id — it must NOT touch B's. Proof: A rebinds immediately AND server B
# is unharmed (after a pump, B still serves on :8081, both answer 200, and
# axl_loop_free reports zero still-active sources). A broken scope would corrupt
# B's deferred ctxs and fault / stop B serving when the loop later pumps them.
#
# AxlTestNet.efi serve-rebind-multi (abortive-only; there is no graceful variant —
# this test is about scope, not the abortive-vs-graceful contrast).
#
# Usage: ./test/integration/test-http-rebind-multi-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
CHURN_N="${CHURN_N:-4}"

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_A=$(test_port 0)
HOST_B=$(test_port 1)

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

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

echo Starting serve-rebind-multi...
AxlTestNet.efi serve-rebind-multi
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== HTTP deferred-close finalize is listener-scoped ($TEST_ARCH, N=$CHURN_N/server) ==="

test_build_qemu_cmd
# Two guest ports (8080, 8081) forwarded on ONE netdev — test_add_port_forward
# would emit a duplicate netdev id 'net0' if called twice.
TEST_QEMU_CMD+=(
    -device "$(_test_nic_device),netdev=net0"
    -netdev "user,id=net0,hostfwd=tcp::${HOST_A}-:8080,hostfwd=tcp::${HOST_B}-:8081"
)
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, ports: $HOST_A->8080  $HOST_B->8081"

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

# On READY: churn BOTH servers so BOTH accumulate deferred closes.
churn() {
  python3 - "$1" "$CHURN_N" << 'PYEOF'
import socket, sys, time
port = int(sys.argv[1]); n = int(sys.argv[2])
socks = []
for _ in range(n):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        try: s.recv(4096)
        except Exception: pass
        socks.append(s)
    except Exception:
        pass
time.sleep(7)
for s in socks:
    try: s.close()
    except Exception: pass
PYEOF
}
churn "$HOST_A" & CA=$!
churn "$HOST_B" & CB=$!

if ! test_wait_for "REBIND-A-RC:" 60; then
    echo "FAIL: no REBIND-A-RC within 60s"; kill "$CA" "$CB" 2>/dev/null
    test_clean_log; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"; exit 1
fi

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log
RC=$(grep -oE 'REBIND-A-RC:-?[0-9]+' "$TEST_CLEAN_LOG" | head -1)
[[ "$RC" == "REBIND-A-RC:0" ]] \
    && pass "server A rebound :8080 while server B had deferred closes on the loop ($RC)" \
    || fail "server A rebind ($RC)"

# After READY2, both A' (:8080) and B (:8081) must serve — B unharmed by A's teardown.
if test_wait_for "READY2" 20; then
    CODE_A=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: close" --max-time 10 "http://127.0.0.1:${HOST_A}/plain" 2>/dev/null || echo 000)
    CODE_B=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: close" --max-time 10 "http://127.0.0.1:${HOST_B}/plain" 2>/dev/null || echo 000)
    [[ "$CODE_A" == "200" ]] && pass "rebound server A' serves on :8080 (200)" || fail "A' GET (got '$CODE_A')"
    [[ "$CODE_B" == "200" ]] && pass "server B STILL serves on :8081 — untouched by A's teardown (200)" || fail "B GET (got '$CODE_B')"
else
    fail "never reached READY2"
fi

test_wait_for "MULTI-DONE" 20 && pass "run completed cleanly (no crash/hang from cross-listener finalize)" || fail "no MULTI-DONE (crash/hang?)"
kill "$CA" "$CB" 2>/dev/null

test_clean_log
grep -q "caller-owned event source .* still active" "$TEST_CLEAN_LOG" \
    && fail "leaked a deferred-close source at loop_free" \
    || pass "zero still-active caller-owned sources at loop_free"

echo ""
printf "listener-scoped deferred-close finalize: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then echo ""; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"; fi
[[ $FAIL -eq 0 && $PASS -eq 5 ]] && exit 0 || exit 1
