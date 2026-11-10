#!/bin/bash
# test-meta: arch=both needs= est=40 local-only=0 guest=standard
# test-tcp-close-pending-qemu.sh -- closing a connection with a send still on
# the wire: caller memory never reaches the wire, and the close is abortive.
#
# THE MECHANISM. axl_tcp_send_async borrows the caller's buffer until its
# callback, but the firmware never does: AxlTcp copies each piece into its own
# buffer before handing it to TcpDxe (which reads it until it has segmented it
# into its own send queue). Before that copy existed, a close that told the
# owner AXL_CANCELLED -- so the owner freed or reused the buffer -- left TcpDxe
# transmitting the owner's new bytes. And a close that drops accepted bytes
# must reset the connection: a FIN would make the truncated stream look
# complete.
#
# THE FIXTURE. AxlTestNet.efi serve-tcp-close-pending sends 16 MiB of 'A' to
# this script's client, which connects with a 4 KiB receive buffer and reads
# NOTHING, so the send stalls against a full window with a piece armed but not
# yet segmented, and the guest closes GRACEFULLY 3 s later. The send callback
# overwrites the buffer with 'Z', prints "cb-stall", and stalls 3 s with the
# firmware's timers running; this script starts the client reading the moment
# it sees that line, so the window OPENS during the stall. A QEMU filter-dump
# capture records what the guest's firmware actually put on the wire,
# independent of slirp's buffering.
#
# THE CONTRACT. No 'Z' (and no byte other than 'A') ever leaves the guest --
# not on the wire, not at the client -- even though the window opens while the
# callback holds its 'Z' buffer. The close is ABORTIVE: the guest sends
# RST. The callback runs only after the firmware has let go of the connection,
# which it observes as its peer-address query failing.
#
# WHAT THE 'Z' CHECK CAN SEE. It is a guard that caller memory never reaches
# the wire. It fails only if BOTH defences are gone -- the copy AND the
# release-before-callback ordering -- which is how its sabotage is run.
#
# CONTROLS: the callback fired once, inside the close, with AXL_CANCELLED; the
# client received SOME 'A' but not all of it (the send really was in flight at
# the close); the capture saw the guest's 'A' payload (the wire detector can
# see bytes at all).
#
# Usage: ./test/integration/test-tcp-close-pending-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080
GUEST_IP=10.0.2.15
SEND_BYTES=$(( 16 * 1024 * 1024 ))

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Its commands are in test-tcp-close-pending-qemu.nsh (booted, test_use_nsh
# appends the reset -s that powers the guest off).
test_use_nsh

test_build_image

echo "=== TCP: close with a send in flight ($TEST_ARCH) ==="

PCAP="$TEST_TMPDIR/tcp-close-pending.pcap"
TRIGGER="$TEST_TMPDIR/tcp-close-pending.go"
CLIENT_OUT="$TEST_TMPDIR/tcp-close-pending.client"
PEER="$(dirname "$0")/tcp-close-peer.py"
rm -f "$PCAP" "$TRIGGER" "$CLIENT_OUT"

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_add_net_dump "$PCAP"
test_run_background

CLIENT_PID=0
# EXIT alone -- common-test.sh's INT/TERM handlers run it (see
# test-tcp-multi-transmit-qemu.sh).
trap 'test_cleanup; [[ $CLIENT_PID -gt 0 ]] && kill $CLIENT_PID 2>/dev/null || true' EXIT

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"

if ! test_wait_boot "READY" 60; then
    echo "FAIL: server did not reach READY within ${TEST_WAITED_S} seconds"
    test_clean_log; echo "--- Serial ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

python3 "$PEER" client "$HOST_PORT" "$TRIGGER" "$CLIENT_OUT" &
CLIENT_PID=$!

test_wait_for "TCPCLOSE: cb-stall" 60 || true
touch "$TRIGGER"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=0

test_wait_for "TCPCLOSE: DONE" 60 || true
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# check <description> <command...>
check() {
    local desc="$1"; shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}
# Exact whole-line occurrences in the guest log.
guest_lines() {
    grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true
}
# Occurrences of a substring (a log line carries a prefix of its own).
guest_count() {
    grep -acF -- "$1" "$TEST_CLEAN_LOG" || true
}
# field <line> <name>: the value of name=VALUE in a summary line.
field() {
    sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<< " $1"
}

test_clean_log

RECV_LINE=$(grep -a '^RECV ' "$CLIENT_OUT" 2>/dev/null || true)
WIRE_LINE=""
WIRE_RC=1
if [[ -s "$PCAP" ]]; then
    WIRE_LINE=$(python3 "$PEER" pcap "$PCAP" "$GUEST_IP" "$GUEST_PORT")
    WIRE_RC=$?
fi
echo "  client: '$RECV_LINE'"
echo "  wire:   '$WIRE_LINE' (parser exit $WIRE_RC)"
grep -a '^TCPCLOSE:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

PROMOTED='close: send(s) pending -- promoted to abortive (RST)'

check "the guest accepted the client (TCPCLOSE: accepted)" \
    test "$(guest_lines 'TCPCLOSE: accepted')" = 1
check "CONTROL: the send callback ran inside the close (TCPCLOSE: closed callbacks=1)" \
    test "$(guest_lines 'TCPCLOSE: closed callbacks=1')" = 1
check "CONTROL: the client got a summary line" \
    test -n "$RECV_LINE"
check "CONTROL: the client received some of the send" \
    test "$(field "$RECV_LINE" a)" -gt 0
check "CONTROL: the send was still in flight at the close (client got fewer than $SEND_BYTES bytes)" \
    test "$(field "$RECV_LINE" a)" -lt "$SEND_BYTES"
check "CONTROL: the wire capture parsed" \
    test "$WIRE_RC" = 0
check "CONTROL: the wire capture sees the guest's payload" \
    test "$(field "$WIRE_LINE" a)" -gt 0
check "the send callback reported CANCELLED after the firmware let go (TCPCLOSE: send-cb status=CANCELLED remote-addr=ERR)" \
    test "$(guest_lines 'TCPCLOSE: send-cb status=CANCELLED remote-addr=ERR')" = 1
check "no byte of the reused buffer left the guest (wire z=0)" \
    test "$(field "$WIRE_LINE" z)" = 0
check "no foreign byte left the guest (wire other=0)" \
    test "$(field "$WIRE_LINE" other)" = 0
check "no byte of the reused buffer reached the client (client z=0)" \
    test "$(field "$RECV_LINE" z)" = 0
check "no foreign byte reached the client (client other=0)" \
    test "$(field "$RECV_LINE" other)" = 0
check "the close was abortive: the guest sent RST" \
    test "$(field "$WIRE_LINE" rst)" -ge 1
check "the close was abortive: the guest sent no FIN" \
    test "$(field "$WIRE_LINE" fin)" = 0
check "the promotion was logged exactly once ($PROMOTED)" \
    test "$(guest_count "$PROMOTED")" = 1
check "the heap recorded no corruption (TCPCLOSE: MEM-CORRUPTION 0)" \
    test "$(guest_lines 'TCPCLOSE: MEM-CORRUPTION 0')" = 1
check "the fixture ran to the end (TCPCLOSE: DONE)" \
    test "$(guest_lines 'TCPCLOSE: DONE')" = 1
check "no leaks at teardown" test_check_leaks

echo ""
printf "TCP close with a send in flight: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
