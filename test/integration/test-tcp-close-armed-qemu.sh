#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 guest=none
# guest=none: written for a filter-dump on net0 and the X64 debugcon, which
#   a pooled guest did not have; both are served pooled now
#   (test_add_net_dump, the pooled X64 debugcon), and this has not been
#   converted yet -- tcp-send-cancel is the model.
# test-tcp-close-armed-qemu.sh -- closing with a FULLY-ARMED send whose last
# Transmit has not completed.
#
# THE CONTRACT (axl-tcp.h, axl_tcp_close). A send whose last piece is already
# handed to the firmware drops nothing when the connection closes: AxlTcp
# copied the piece, and TCP sends it ahead of the FIN. So:
#   graceful -- a GRACEFUL close with only that send outstanding stays
#     graceful (FIN, no RST), retires it AXL_OK with its whole size, and the
#     peer receives every byte the guest reported sent.
#   abortive -- with a send QUEUED behind it the close drops accepted bytes
#     and is promoted to RST; the RST discards the armed piece, so that send
#     is retired AXL_CANCELLED with its whole size still counted as sent,
#     and the queued one AXL_CANCELLED with 0.
#
# THE FIXTURE. AxlTestNet.efi serve-tcp-close-armed MODE sends one-piece
# sends back to back to a peer that reads nothing (tcp-close-peer.py client
# client) until one stops completing -- stuck, fully armed, against the
# full window -- then closes. Both modes run in one boot, on ports 8080
# (graceful) and 8081 (abortive).
#
# Usage: ./test/integration/test-tcp-close-armed-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT_G=$(test_port 0)
HOST_PORT_A=$(test_port 1)
GUEST_IP=10.0.2.15
PIECE=8192

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all tests 2>&1 | tail -2

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

AxlTestNet.efi serve-tcp-close-armed graceful
AxlTestNet.efi serve-tcp-close-armed abortive
stall 1000000
reset -s
NSHEOF

test_build_image

echo "=== TCP: close with a fully-armed send ($TEST_ARCH) ==="

PCAP="$TEST_TMPDIR/tcp-close-armed.pcap"
PEER="$(dirname "$0")/tcp-close-peer.py"
rm -f "$PCAP" "$TEST_TMPDIR"/armed-*

# Both closes run with a Transmit still armed -- the TcpDxe paths most likely
# to trip SockDataSent -- so the firmware's DEBUG output is checked too.
test_capture_firmware_debug
test_build_qemu_cmd
test_add_port_forward "$HOST_PORT_G" 8080 "$HOST_PORT_A" 8081
TEST_QEMU_CMD+=(-object "filter-dump,id=axl_dump0,netdev=net0,file=$PCAP")
test_run_background

CLIENT_PID=0
trap 'test_cleanup; [[ $CLIENT_PID -gt 0 ]] && kill $CLIENT_PID 2>/dev/null || true' EXIT

echo "  QEMU PID: $TEST_QEMU_PID, ports: $HOST_PORT_G -> 8080, $HOST_PORT_A -> 8081"

# run_mode MODE HOST_PORT: one client through one fixture run.
# One guest runs both modes: the first READY covers its boot (scaled limit), the
# second is a bounded wait on a guest that is already up.
_ready_wait=test_wait_boot
run_mode() {
    local mode="$1" port="$2"
    if ! $_ready_wait "READY $mode" 60; then
        echo "FAIL: $mode fixture did not reach READY within ${TEST_WAITED_S}s"
        return 1
    fi
    _ready_wait=test_wait_for
    python3 "$PEER" client "$port" "$TEST_TMPDIR/armed-$mode.go" \
        "$TEST_TMPDIR/armed-$mode.client" &
    CLIENT_PID=$!
    test_wait_for "TCPARMED $mode: closed" 60 || true
    touch "$TEST_TMPDIR/armed-$mode.go"
    wait "$CLIENT_PID" 2>/dev/null || true
    CLIENT_PID=0
    test_wait_for "TCPARMED $mode: DONE" 60 || true
}
run_mode graceful "$HOST_PORT_G"
run_mode abortive "$HOST_PORT_A"
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local desc="$1"; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}
guest_lines() {
    grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true
}
guest_count() {
    grep -acF -- "$1" "$TEST_CLEAN_LOG" || true
}
guest_field() {
    local line
    line=$(grep -a -m1 -F -- "$1" "$TEST_CLEAN_LOG" || true)
    field "$line" "$2"
}
field() {
    sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<< " $1"
}

test_clean_log
grep -a '^TCPARMED' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

G_RECV=$(grep -a '^RECV ' "$TEST_TMPDIR/armed-graceful.client" 2>/dev/null || true)
A_RECV=$(grep -a '^RECV ' "$TEST_TMPDIR/armed-abortive.client" 2>/dev/null || true)
G_WIRE=""
A_WIRE=""
if [[ -s "$PCAP" ]]; then
    G_WIRE=$(python3 "$PEER" pcap "$PCAP" "$GUEST_IP" 8080)
    A_WIRE=$(python3 "$PEER" pcap "$PCAP" "$GUEST_IP" 8081)
fi
echo "  graceful client: '$G_RECV'"
echo "  graceful wire:   '$G_WIRE'"
echo "  abortive client: '$A_RECV'"
echo "  abortive wire:   '$A_WIRE'"

G_TOTAL=$(guest_field 'TCPARMED graceful: closed' total-sent)
PROMOTED='close: send(s) pending -- promoted to abortive (RST)'

# graceful
check "graceful: CONTROL -- sends completed before one stuck" \
    test "$(guest_field 'TCPARMED graceful: stuck' completed)" -gt 0
check "graceful: the stuck send is fully armed (queued 0)" \
    test "$(guest_field 'TCPARMED graceful: stuck' queued)" = 0
check "graceful: the close retires the stuck send AXL_OK with its whole piece" \
    test "$(guest_lines "TCPARMED graceful: armed-cb status=OK sent=$PIECE")" = 1
check "graceful: the peer received every byte reported sent ($G_TOTAL)" \
    test -n "$G_TOTAL" -a "$(field "$G_RECV" a)" = "$G_TOTAL"
check "graceful: the stream ended with end-of-stream, not a reset" \
    test "$(field "$G_RECV" end)" = eof
check "graceful: the guest sent FIN" \
    test "$(field "$G_WIRE" fin)" -ge 1
check "graceful: the guest sent no RST" \
    test "$(field "$G_WIRE" rst)" = 0
# abortive
check "abortive: the stuck send is fully armed (queued 0)" \
    test "$(guest_field 'TCPARMED abortive: stuck' queued)" = 0
check "abortive: the queued send counts in full" \
    test "$(guest_lines 'TCPARMED abortive: queued-after-q=4096')" = 1
check "abortive: the queued send is CANCELLED with 0 bytes sent" \
    test "$(guest_lines 'TCPARMED abortive: q-cb status=CANCELLED sent=0')" = 1
check "abortive: the fully-armed send is CANCELLED with its whole piece sent" \
    test "$(guest_lines "TCPARMED abortive: armed-cb status=CANCELLED sent=$PIECE")" = 1
check "abortive: the guest sent RST" \
    test "$(field "$A_WIRE" rst)" -ge 1
check "abortive: the guest sent no FIN" \
    test "$(field "$A_WIRE" fin)" = 0
check "abortive: no byte of the queued send left the guest (wire q=0)" \
    test "$(field "$A_WIRE" q)" = 0
check "only the abortive close was promoted (logged exactly once)" \
    test "$(guest_count "$PROMOTED")" = 1
test_check_firmware_asserts check TcpDxe
check "both fixtures ran to the end" \
    test "$(guest_lines 'TCPARMED graceful: DONE')/$(guest_lines 'TCPARMED abortive: DONE')" = 1/1
check "no leaks at teardown" test_check_leaks

echo ""
printf "TCP close with a fully-armed send: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
