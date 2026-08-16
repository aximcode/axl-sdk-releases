#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=0
# axl-kernel third SoftBMC-port test — runs axlk-reqlog-server inside
# QEMU and drives it with curl from the host. Validates two things:
#
#  1) Live RAM-resident state (8-entry ring mutated by every request,
#     with observable cross-request side effects). This is the shape
#     the HwInfo and BootConfig ports didn't cover.
#
#  2) That the accept-then-spawn pattern runs unbounded thanks to
#     inline WNOHANG zombie draining in the server. Test issues 24
#     connections against a 16-slot PCB — pre-WNOHANG this would
#     deadlock around connection 14.
#
# Usage: ./test/integration/test-axlk-reqlog.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8082
RING_CAP=8
MAX_CLIENTS=24

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all axlk-reqlog-server 2>&1 | tail -3

TEST_BUILD_DIR="$(test_build_dir)"
test_add_efi "$TEST_BUILD_DIR/axlk-reqlog-server.efi"

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

echo Starting axlk-reqlog-server...
axlk-reqlog-server.efi
NSHEOF

test_build_image

echo "=== axl-kernel ReqLog Port ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server..."

if ! test_wait_for "listening on port" 60; then
    echo "FAIL: axlk-reqlog-server did not start within 60s"
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

# pipefail-safe extractors. grep returns 1 on no match — without `|| true`
# the whole pipeline aborts under `set -euo pipefail` and the script dies
# silently mid-test.
extract_field() {
    local body="$1" key="$2" v
    v=$(echo "$body" | grep -oE "\"${key}\":[0-9]+" | head -1 | cut -d: -f2 || true)
    echo "${v:-0}"
}

count_entries() {
    echo "$1" | { grep -oE '"ts_ms":[0-9]+' || true; } | wc -l
}

# ---------------------------------------------------------------------------
# Phase 1 — sanity. Server is fresh; the first request mutates state to
# recv=1, head=1.
# ---------------------------------------------------------------------------
out=$(fetch "/")
code=$(echo "$out" | tail -1)
body=$(echo "$out" | head -n -1)
if [[ "$code" == "200" && "$body" == *"\"capacity\":${RING_CAP}"* ]]; then
    echo "  PASS: initial /  (capacity=${RING_CAP}; recv=$(extract_field "$body" received))"
    echo "    body: $body"
else
    echo "  FAIL: initial / (HTTP $code)"
    echo "    body: $body"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# Phase 2 — fill ring under capacity. After 5 healthz + 1 /log, ring holds
# 7 entries (cap is 8, so still no drops). /log records itself, so the
# response should include it as the newest entry.
# ---------------------------------------------------------------------------
for i in 1 2 3 4 5; do fetch "/healthz" >/dev/null; done

out=$(fetch "/log")
body=$(echo "$out" | head -n -1)
n=$(count_entries "$body")
if [[ "$n" == "7" ]]; then
    echo "  PASS: under-capacity /log returned 7 entries"
else
    echo "  FAIL: under-capacity /log expected 7 entries, got $n"
    echo "    body: $body"
    FAIL=$((FAIL + 1))
fi

if [[ "$body" == *'"path":"/log"'* ]]; then
    echo "  PASS: newest entry is /log (request records itself)"
else
    echo "  FAIL: /log entry not present in /log response"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# Phase 3 — push past capacity. Already at recv=7. Pump 3 more healthz
# (recv=10) then query / (recv=11). Expected: dropped = 11 - 8 = 3.
# ---------------------------------------------------------------------------
for i in 1 2 3; do fetch "/healthz" >/dev/null; done

out=$(fetch "/")
body=$(echo "$out" | head -n -1)
recv=$(extract_field "$body" received)
drop=$(extract_field "$body" dropped)
if [[ "$recv" == "11" && "$drop" == "3" ]]; then
    echo "  PASS: post-wrap / (recv=11, dropped=3)"
else
    echo "  FAIL: post-wrap / expected recv=11 drop=3, got recv=$recv drop=$drop"
    echo "    body: $body"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# Phase 4 — /log after wrap should hold exactly RING_CAP entries.
# That's the bounded-memory invariant.
# ---------------------------------------------------------------------------
out=$(fetch "/log")
body=$(echo "$out" | head -n -1)
n=$(count_entries "$body")
if [[ "$n" == "$RING_CAP" ]]; then
    echo "  PASS: post-wrap /log holds exactly $RING_CAP entries"
else
    echo "  FAIL: post-wrap /log expected $RING_CAP entries, got $n"
    echo "    body: $body"
    FAIL=$((FAIL + 1))
fi

if [[ "$body" == *'"path":"/log"'* ]]; then
    echo "  PASS: newest entry is /log after wrap"
else
    echo "  FAIL: /log entry missing from post-wrap /log response"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# Phase 5 — unbounded-connection check. Pre-WNOHANG the service caps out
# around connection 14 because handler zombies fill the 16-slot PCB.
# With WNOHANG zombie draining in the accept loop, slots recycle and we
# can run past that cap. Pump 10 more /healthz (recv=22, drop=14).
# ---------------------------------------------------------------------------
for i in 1 2 3 4 5 6 7 8 9 10; do fetch "/healthz" >/dev/null; done

out=$(fetch "/")
body=$(echo "$out" | head -n -1)
recv=$(extract_field "$body" received)
drop=$(extract_field "$body" dropped)
# After this overview request: recv=23, drop=23-8=15
if [[ "$recv" == "23" && "$drop" == "15" ]]; then
    echo "  PASS: past-old-PCB-cap / (recv=23, drop=15; no deadlock)"
else
    echo "  FAIL: past-PCB / expected recv=23 drop=15, got recv=$recv drop=$drop"
    echo "    body: $body"
    FAIL=$((FAIL + 1))
fi

# ---------------------------------------------------------------------------
# Phase 6 — final /log. Ring still holds exactly RING_CAP entries after
# 24 total connections.
# ---------------------------------------------------------------------------
out=$(fetch "/log")
body=$(echo "$out" | head -n -1)
n=$(count_entries "$body")
if [[ "$n" == "$RING_CAP" ]]; then
    echo "  PASS: /log after 24 connections still holds $RING_CAP entries"
else
    echo "  FAIL: final /log expected $RING_CAP entries, got $n"
    echo "    body: $body"
    FAIL=$((FAIL + 1))
fi

# Connection accounting:
#   1 (init /) + 5 (healthz) + 1 (/log)      = 7    (phase 1+2)
# + 3 (healthz) + 1 (/) + 1 (/log)           = 12   (phase 3+4)
# +10 (healthz) + 1 (/) + 1 (/log)           = 24   (phase 5+6)
# = MAX_CLIENTS. Server's accept loop exits, then reaps children.

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
check_log "served-clients"  "^PASS: axlk-reqlog-server"
check_log "clean-exit"      "kernel exited rc=0"

echo ""
printf "axl-kernel ReqLog: FAIL=%d (%s)\n" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- Serial log (tail) ---"
    tail -60 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
