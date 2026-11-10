#!/bin/bash
# test-meta: arch=both needs=ssh est=40 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-send-teardown-qemu.sh -- a connection torn down while a FINISHED
# send's callback is still owed.
#
# axl_tcp_close delivers every send callback it still holds before it
# returns: the ones on the wire or queued with AXL_CANCELLED, and one that
# had already FINISHED -- retired, its callback scheduled for the next loop
# iteration but not yet run -- with the status it finished with (AXL_OK or
# AXL_ERR). The server's on_sent once treated only AXL_CANCELLED as "this
# connection is being torn down", so a finished send delivered from inside
# conn_drop's close flushed the connection's pending packets, found the
# socket closed, and dropped the connection AGAIN from inside its own drop:
# penalty accrued twice and conn_free ran twice on one connection.
#
# THE WINDOW IS ONE LOOP ITERATION WIDE in the ordinary case, so it is held
# open with a seam rather than hunted by timing: the fixture's holdsend=1
# (axl_tcp_fail_next_send_schedule) makes this boot's first finished send
# take the path AxlTcp takes when the defer queue AND source table are both
# full -- its callback then runs only when the socket is closed.
#
# ONE SCENARIO, held-send-drop. That first send is the new connection's
# identification line and KEXINIT: the peer (ssh-peer.py kexwait --stage
# ecdh) receives both, answers with its KEXINIT and KEX_ECDH_INIT, and reads
# nothing more. The server seals KEX_ECDH_REPLY and NEWKEYS, which WAIT in
# its transmit queue behind the send whose callback is held -- one send in
# flight per connection. The peer then closes, and on_recv's conn_drop runs
# with exactly that state: a finished send owed, AXL_OK, frames pending.
#
# WHAT IT DISCRIMINATES: on the defective code the guest logs the re-entry
# ("conn_drop re-entered", graded by hostile_finish in every hostile-lib
# boot) and, without that guard, the double free crashed the guest in
# conn_free. The heartbeat, deadline, leak and corruption checks come from
# hostile_finish too. The PRECONDITIONS pin that the state really arose:
# the peer saw nothing after KEXINIT, the seam fired once and was not a
# real both-pools-full hold, and conn_drop saw the two frames queued.
#
# Usage: ./test/integration/test-ssh-send-teardown-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh send teardown"
HOSTILE_SCENARIOS="held-send-drop"

# The peer's key exchange and 2 s hold, then the liveness login.
declare -A HOSTILE_BUDGET=([held-send-drop]=8)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
HOSTILE_EXTRA_ARGS="sshloglevel=debug holdsend=1"
PEER="$TESTS_DIR/ssh-peer.py"

hostile_boot "AxlSsh send teardown"

# The seam's own line (tcp, DEBUG -- holdsend=N raises that domain) and the
# REAL both-pools-full error, which an injected hold must never print.
HELD_INJECTED='async send: completion held (injected by axl_tcp_fail_next_send_schedule)'
HELD_REAL='async send: cannot schedule the completion callback'
# conn_drop's DEBUG line for exactly the state under test: KEX_ECDH_REPLY and
# NEWKEYS queued behind the held identification+KEXINIT send.
DROP_QUEUED='drop: 2 frame(s) queued behind a send in flight'

# --- held-send-drop ----------------------------------------------------------
if wants held-send-drop; then
    HS_OUT="$TEST_TMPDIR/held-send-drop.out"
    HS_RC=0
    python3 "$PEER" kexwait 127.0.0.1 "$HOST_PORT" --stage ecdh --hold 2 \
        > "$HS_OUT" 2>&1 || HS_RC=$?
    # The peer's close reaches the guest as an EOF on_recv acts on; give the
    # drop a moment to be logged before the log is read.
    sleep 1
    test_clean_log
    echo "  held-send-drop: peer '$(tr '\n' '|' < "$HS_OUT")' rc=$HS_RC;" \
         "injected holds $(guest_count "$HELD_INJECTED");" \
         "real both-full $(guest_count "$HELD_REAL");" \
         "queued drops $(guest_count "$DROP_QUEUED")"

    # rx=0 is what says the held send was THIS connection's first: had the
    # seam held any other send, the identification+KEXINIT callback would
    # have run, the reply would have left, and the peer would count its bytes.
    check "held-send-drop PRECONDITION: the peer got the identification line and KEXINIT, sent KEX_ECDH_INIT, and then received 0 bytes" \
        peer_is "$HS_RC" 0 "$HS_OUT" 'peer: ecdh init sent' 'peer: held rx=0'
    check "held-send-drop PRECONDITION: the seam held exactly one completion" \
        test "$(guest_count "$HELD_INJECTED")" = 1
    check "held-send-drop CONTROL: no REAL both-pools-full hold happened" \
        test "$(guest_count "$HELD_REAL")" = 0
    check "held-send-drop PRECONDITION: the drop found 2 frames queued behind a send in flight (exactly once)" \
        test "$(guest_count "$DROP_QUEUED")" = 1
    check "held-send-drop: the server still answers a login afterwards" \
        good_login_works held-send-drop
fi

hostile_finish
