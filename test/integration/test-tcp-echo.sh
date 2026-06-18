#!/bin/bash
# test-meta: arch=x64 needs= est=18 local-only=0
# tcp-echo integration test — minimal TCP-only repro for axl_tcp_close
# behavior under sequential connect/disconnect storm.
#
# Boots sdk/examples/tcp-echo-server.efi inside UEFI, then runs N
# sequential probes from the host through the QEMU port forward and
# checks two things:
#
#   1. Every probe gets its echo back (server functional).
#   2. After the storm, the host-side QEMU NAT slot table has only
#      LISTEN + (curl-side) TIME-WAIT — NO accumulating CLOSE-WAIT
#      or FIN-WAIT-2 indicating the server's FIN never made it out.
#
# Why a separate test:
#   test-http exercises ~50 mixed-shape requests against an HTTP
#   server (cache, routes, auth, upload, WS). That hides the raw
#   axl_tcp_close behavior under HTTP-server complexity. This test
#   is one .c file (130 lines), one syscall pattern, one close path
#   per probe — much faster GDB iteration when chasing TCP bugs.
#
# Usage: ./test/integration/test-tcp-echo.sh [--arch X64|AARCH64] [--probes N]

source "$(dirname "$0")/common-test.sh"

# --probes N is local to this script; strip it before test_parse_args.
PROBES=15
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --probes) PROBES="$2"; shift 2 ;;
        *)        ARGS+=("$1"); shift ;;
    esac
done
test_parse_args "${ARGS[@]}"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=7000
ECHO_PREFIX="msg"

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

BUILD_LOG=$(mktemp)
if ! make -C "$PROJECT_DIR" \
        ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
        all tcp-echo-server > "$BUILD_LOG" 2>&1; then
    echo "FAIL: build failed for tcp-echo-server"
    cat "$BUILD_LOG"
    rm -f "$BUILD_LOG"
    exit 1
fi
tail -3 "$BUILD_LOG"
rm -f "$BUILD_LOG"

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/tcp-echo-server.efi"

# Startup: connect drivers, DHCP, run server.
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

echo Starting tcp-echo-server...
tcp-echo-server.efi
NSHEOF

test_build_image

echo "=== tcp-echo Integration Test ($TEST_ARCH, probes=$PROBES) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server to start..."

if ! test_wait_for "tcp-echo-server: listening" 60; then
    echo "FAIL: tcp-echo-server did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"
    tail -30 "$TEST_CLEAN_LOG"
    echo "---"
    exit 1
fi

echo "  Server is ready"
sleep 2

# Sequential probe storm: each probe is a fresh TCP connection that
# sends a unique message, half-closes, drains the echo, fully closes.
# We use tcp-probe.py (Python sockets) for portability across nc
# variants (openbsd-nc has -q, nmap-ncat does not).
PROBE_TIMEOUT=2
ECHO_OK=0
ECHO_FAIL=0
ECHO_FAIL_DETAILS=()

echo "  Running $PROBES probes (timeout ${PROBE_TIMEOUT}s each)..."
PROBE_START=$(date +%s)
PROBE_ERR_LOG=$(mktemp)
for i in $(seq 1 "$PROBES"); do
    msg="${ECHO_PREFIX}${i}"
    reply=$(timeout $((PROBE_TIMEOUT + 2)) python3 \
        "$(dirname "$0")/tcp-probe.py" \
        127.0.0.1 "$HOST_PORT" "$msg" "$PROBE_TIMEOUT" \
        2>>"$PROBE_ERR_LOG" || true)
    if [[ "$reply" == "$msg" ]]; then
        ECHO_OK=$((ECHO_OK + 1))
    else
        err_tail=$(tail -1 "$PROBE_ERR_LOG" 2>/dev/null || true)
        ECHO_FAIL=$((ECHO_FAIL + 1))
        ECHO_FAIL_DETAILS+=("#$i sent='$msg' got='$reply' err='$err_tail'")
    fi
done
rm -f "$PROBE_ERR_LOG"
PROBE_ELAPSED=$(( $(date +%s) - PROBE_START ))
echo "  Probes done in ${PROBE_ELAPSED}s"

# Snapshot host TCP state — ANY CLOSE-WAIT / FIN-WAIT-{1,2} on the
# 17000 port pair is a leak (server's FIN never made it out, or
# slirp lost track of a connection). After the storm we expect:
# 1 LISTEN + N TIME-WAIT (curl-side normal client teardown).
# `grep -c` returns 1 when no matches — under `set -euo pipefail`
# (inherited from common-test.sh) that silently kills the script
# right when the test SUCCEEDS. Wrap to absorb.
SS_OUT=$(ss -ant '( sport = :'"$HOST_PORT"' or dport = :'"$HOST_PORT"' )' | tail -n +2)
HOST_TCP_TOTAL=$(printf '%s\n' "$SS_OUT" | grep -cv '^$' || true)
HOST_TCP_BREAKDOWN=$(printf '%s\n' "$SS_OUT" | awk 'NF{print $1}' | sort | uniq -c | tr '\n' ' ')
HOST_TCP_LEAKED=$(printf '%s\n' "$SS_OUT" | awk 'NF{print $1}' | { grep -cE 'CLOSE-WAIT|FIN-WAIT' || true; })

# Stop QEMU. Don't `wait` — bash sometimes hangs on it when the
# child was started inside a sourced helper. The EXIT trap will
# reap.
sleep 1
echo "  Stopping QEMU..."
kill "$TEST_QEMU_PID" 2>/dev/null || true
sleep 1
kill -9 "$TEST_QEMU_PID" 2>/dev/null || true
TEST_QEMU_PID=0

test_clean_log

# Count guest-side activity for sanity — should match probe count.
# `axl_printf("  recv: %s", buf)` doesn't add a newline (the buf may
# or may not contain one), so subsequent "connected:" prints land on
# the same line. Count occurrences anywhere, not just line-anchored.
# `|| true` because `set -euo pipefail` is on and `grep -c` returns
# 1 when nothing matches — which is exactly the failure mode we want
# to REPORT, not abort on.
GUEST_CONNECTS=$( ( grep -o 'connected:' "$TEST_CLEAN_LOG" 2>/dev/null || true ) | wc -l )
GUEST_DISCONNECTS=$( ( grep -o 'disconnected' "$TEST_CLEAN_LOG" 2>/dev/null || true ) | wc -l )

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

[[ $ECHO_OK -eq $PROBES ]] \
    && pass "all $PROBES probes echoed correctly" \
    || fail "$ECHO_FAIL/$PROBES probes failed: ${ECHO_FAIL_DETAILS[*]:-none}"

[[ $GUEST_CONNECTS -ge $PROBES ]] \
    && pass "guest logged $GUEST_CONNECTS connects" \
    || fail "guest only logged $GUEST_CONNECTS connects (expected >= $PROBES)"

[[ $GUEST_DISCONNECTS -ge $PROBES ]] \
    && pass "guest logged $GUEST_DISCONNECTS disconnects" \
    || fail "guest only logged $GUEST_DISCONNECTS disconnects (expected >= $PROBES)"

# The headline check: any CLOSE-WAIT / FIN-WAIT post-storm is a leak.
[[ $HOST_TCP_LEAKED -eq 0 ]] \
    && pass "host TCP clean (no CLOSE-WAIT/FIN-WAIT leaks)" \
    || fail "host TCP leaked $HOST_TCP_LEAKED stuck sockets — server FIN not delivered. Breakdown: $HOST_TCP_BREAKDOWN"

echo ""
printf "tcp-echo tests: %d passed, %d failed (%s, probes=%d)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH" "$PROBES"
echo "  Host TCP after storm: $HOST_TCP_TOTAL sockets — $HOST_TCP_BREAKDOWN"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log (last 40 lines) ---"
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
