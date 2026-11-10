#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 guest=standard
# test-tcp-send-cancel-qemu.sh -- cancelling a send the firmware already holds.
#
# THE MECHANISM. TcpDxe reads a Transmit's data until it has segmented it into
# its own send queue, and completes the token then. EFI_TCP4.Cancel on a
# Transmit it is still processing (SockCancelToken) frees the token but leaves
# the data queued; the next segmentation's SockDataSent then ASSERTs on a DEBUG
# firmware (SockImpl.c, ProcessingSndTokenList empty) or signals a freed token
# on a RELEASE one. So AxlTcp never cancels an armed Transmit: a cancel stops a
# send at a PIECE boundary, the armed piece (the "orphan piece") goes out on its
# own from AxlTcp's own copy of it, and the next send waits for it.
#
# THE FIXTURE. AxlTestNet.efi serve-tcp-send-cancel walks, on one connection
# to a peer that reads only when told (tcp-close-peer.py cancel-client):
# cancel a QUEUED send, cancel the ACTIVE send mid-stream, send a marker
# behind its orphan piece, exchange PONG, time out a SYNCHRONOUS send against
# the stalled peer, send a second marker behind THAT orphan from the main
# loop, exchange PONG2, and close gracefully. Every callback overwrites its
# buffer with 'Z' at once. See the fixture's comment block for the protocol.
#
# THE CONTRACT.
#   - No firmware ASSERT (x64: OVMF's debugcon; aa64: its debug UART; both:
#     the serial log).
#   - Byte-exact stream: exactly the cancelled send's reported count of 'A',
#     then "MARK", then exactly the timed-out sync send's count of 'B', then
#     "MRK2", then nothing. Each count is a whole number of pieces.
#   - No 'Z' (caller memory after its owner got it back) and no 'Q' (the
#     cancelled queued send) anywhere -- not on the wire, not at the peer.
#   - Exact queued-bytes readings at the points the fixture prints them.
#   - The connection stays usable (PONG and PONG2 arrive), and the final
#     graceful close is abortive (RST, no FIN) because the stream was cut
#     short.
#
# Usage: ./test/integration/test-tcp-send-cancel-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080
GUEST_IP=10.0.2.15
BIG_BYTES=$(( 16 * 1024 * 1024 ))
Q_BYTES=$(( 64 * 1024 ))
CHUNK_BYTES=8192

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" all tests 2>&1 | tail -2

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Its commands are in test-tcp-send-cancel-qemu.nsh (booted, test_use_nsh
# appends the reset -s that powers the guest off).
test_use_nsh

test_build_image

echo "=== TCP: cancelling a send the firmware holds ($TEST_ARCH) ==="

PCAP="$TEST_TMPDIR/tcp-send-cancel.pcap"
TRIGGER1="$TEST_TMPDIR/tcp-send-cancel.go1"
TRIGGER2="$TEST_TMPDIR/tcp-send-cancel.go2"
CLIENT_OUT="$TEST_TMPDIR/tcp-send-cancel.client"
PEER="$(dirname "$0")/tcp-close-peer.py"
rm -f "$PCAP" "$TRIGGER1" "$TRIGGER2" "$CLIENT_OUT"

# Re-adding the Cancel shows "ASSERT [TcpDxe] ...LinkedList.c(594)" in the
# firmware's DEBUG output (x64: OVMF's debugcon, which every pooled X64 guest
# has; aa64: its debug UART) -- see test_capture_firmware_debug.
test_capture_firmware_debug

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

python3 "$PEER" cancel-client "$HOST_PORT" "$TRIGGER1" "$TRIGGER2" "$CLIENT_OUT" &
CLIENT_PID=$!

test_wait_for "TCPCANCEL: queued-after-marker=" 60 || true
touch "$TRIGGER1"
test_wait_for "TCPCANCEL: queued-after-marker2=" 60 || true
touch "$TRIGGER2"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=0

test_wait_for "TCPCANCEL: DONE" 60 || true
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
# The value of name=VALUE in the first guest line starting with PREFIX.
guest_field() {
    local line
    line=$(grep -a -m1 -F -- "$1" "$TEST_CLEAN_LOG" || true)
    field "$line" "$2"
}
field() {
    sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<< " $1"
}
whole_pieces() {
    [[ "$1" =~ ^[0-9]+$ ]] && (( $1 > 0 && $1 < BIG_BYTES && $1 % CHUNK_BYTES == 0 ))
}

test_clean_log

STREAM_LINE=$(grep -a '^STREAM ' "$CLIENT_OUT" 2>/dev/null || true)
WIRE_LINE=""
WIRE_RC=1
if [[ -s "$PCAP" ]]; then
    WIRE_LINE=$(python3 "$PEER" pcap "$PCAP" "$GUEST_IP" "$GUEST_PORT")
    WIRE_RC=$?
fi
echo "  client: '$STREAM_LINE'"
echo "  wire:   '$WIRE_LINE' (parser exit $WIRE_RC)"
grep -a '^TCPCANCEL:' "$TEST_CLEAN_LOG" | sed 's/^/    guest: /' || true

A_SENT=$(guest_field 'TCPCANCEL: a-cb ' sent)
SYNC_SENT=$(guest_field 'TCPCANCEL: sync ' sent)
PROMOTED='close: send(s) pending -- promoted to abortive (RST)'

check "the guest accepted the client" \
    test "$(guest_lines 'TCPCANCEL: accepted')" = 1
check "queued bytes after submitting A and Q: A's unarmed rest plus all of Q" \
    test "$(guest_lines "TCPCANCEL: queued-after-submit=$(( BIG_BYTES - CHUNK_BYTES + Q_BYTES ))")" = 1
check "cancelling the QUEUED send: CANCELLED, 0 bytes sent, A still unarmed-heavy" \
    test "$(guest_field 'TCPCANCEL: q-cb ' status)/$(guest_field 'TCPCANCEL: q-cb ' sent)" = "CANCELLED/0"
check "cancelling the ACTIVE send: CANCELLED" \
    test "$(guest_field 'TCPCANCEL: a-cb ' status)" = CANCELLED
check "cancelling the ACTIVE send: a whole number of pieces sent, not all ($A_SENT)" \
    whole_pieces "$A_SENT"
check "after the active cancel only the orphan piece is left: queued 0" \
    test "$(guest_field 'TCPCANCEL: a-cb ' queued)" = 0
check "a send behind the orphan piece waits unarmed: queued 4" \
    test "$(guest_lines 'TCPCANCEL: queued-after-marker=4')" = 1
check "the marker completes: OK, 4 bytes" \
    test "$(guest_lines 'TCPCANCEL: marker-cb status=OK sent=4')" = 1
check "the connection stays usable after the cancel (PONG received)" \
    test "$(guest_lines 'TCPCANCEL: pong')" = 1
check "the sync send against the stalled peer returns TIMEOUT" \
    test "$(guest_field 'TCPCANCEL: sync ' rc)" = TIMEOUT
check "the sync send reports a whole number of pieces, not all ($SYNC_SENT)" \
    whole_pieces "$SYNC_SENT"
check "a main-loop send behind the sync call's orphan waits unarmed: queued 4" \
    test "$(guest_lines 'TCPCANCEL: queued-after-marker2=4')" = 1
check "the second marker completes: OK, 4 bytes" \
    test "$(guest_lines 'TCPCANCEL: marker2-cb status=OK sent=4')" = 1
check "the connection stays usable after the timeout (PONG2 received)" \
    test "$(guest_lines 'TCPCANCEL: pong2')" = 1
check "CONTROL: the client got a summary line" \
    test -n "$STREAM_LINE"
check "the peer got exactly the reported count of 'A', then MARK" \
    test "$(field "$STREAM_LINE" a)/$(field "$STREAM_LINE" mark)" = "$A_SENT/1"
check "the peer got exactly the reported sync count of 'B', then MRK2" \
    test "$(field "$STREAM_LINE" b)/$(field "$STREAM_LINE" mrk2)" = "$SYNC_SENT/1"
check "nothing followed MRK2" \
    test "$(field "$STREAM_LINE" rest)" = 0
check "no caller byte after its callback reached the peer (client z=0)" \
    test "$(field "$STREAM_LINE" z)" = 0
check "no byte of the cancelled queued send reached the peer (client q=0)" \
    test "$(field "$STREAM_LINE" q)" = 0
check "CONTROL: the wire capture parsed and saw the guest's payload" \
    test "$WIRE_RC" = 0 -a "$(field "$WIRE_LINE" a)" -gt 0
check "no caller byte after its callback left the guest (wire z=0)" \
    test "$(field "$WIRE_LINE" z)" = 0
check "no byte of the cancelled queued send left the guest (wire q=0)" \
    test "$(field "$WIRE_LINE" q)" = 0
check "the truncated stream's graceful close was abortive: RST" \
    test "$(field "$WIRE_LINE" rst)" -ge 1
check "the truncated stream's graceful close was abortive: no FIN" \
    test "$(field "$WIRE_LINE" fin)" = 0
check "the promotion was logged exactly once ($PROMOTED)" \
    test "$(guest_count "$PROMOTED")" = 1
test_check_firmware_asserts check TcpDxe
check "the heap recorded no corruption (TCPCANCEL: MEM-CORRUPTION 0)" \
    test "$(guest_lines 'TCPCANCEL: MEM-CORRUPTION 0')" = 1
check "the fixture ran to the end (TCPCANCEL: DONE)" \
    test "$(guest_lines 'TCPCANCEL: DONE')" = 1
check "no leaks at teardown" test_check_leaks

echo ""
printf "TCP send cancel: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -40 "$TEST_CLEAN_LOG"
    if [[ -n "${TEST_FW_DEBUGCON:-}" ]]; then
        echo "--- firmware DEBUG ASSERT lines ---"; grep -a "ASSERT" "$TEST_FW_DEBUGCON" | head -5
    fi
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
