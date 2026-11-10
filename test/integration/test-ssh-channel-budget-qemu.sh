#!/bin/bash
# test-meta: arch=both needs=ssh est=53 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-channel-budget-qemu.sh -- the server-wide write total, the
# fair-share floor and the cross-connection writable wake of AxlSsh session
# channels, inside a real server: two connections driven by ssh-peer.py.
#
# The unit tests pin the write table's arithmetic (AxlTestSsh "chan budget"
# groups); what only a running server can show is the WIRING -- that bytes
# leaving one connection reach a channel on ANOTHER connection that a
# refusal on the total left waiting. At the shipped 4 MB total no scenario's
# output reaches the total at all, so this file boots its own fixture with
# `writelimit=98304,131072` (ssh-server-selftest.c's option list): a 96 KB
# per-channel limit, a 128 KB total, and so a floor of 131072 / 160 = 819
# bytes. It cannot share a boot with test-ssh-channel-hostile-qemu.sh, whose
# zero-window scenario pins the SHIPPED 256 KB per-channel limit.
#
# Setup and teardown checks are lib/ssh-hostile-lib.sh's; the peer helpers
# and message bodies are lib/ssh-channel-lib.sh's.
#
# Scenarios:
#   total-and-floor   connection A holds the whole total on two channels
#                     whose peer granted no window; on connection B a write
#                     no larger than the floor is still accepted, a larger
#                     one is refused on the total, and it is woken -- once
#                     -- by A's bytes leaving, not by anything B does
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-channel-budget-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh channel budget"
HOSTILE_SCENARIOS="total-and-floor"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh). total-and-floor is mostly its own fixed waits:
# A's 8 s hold before its adjust and 4 s after its CLOSE arrives, plus four
# logins' worth of pure-Python crypto.
declare -A HOSTILE_BUDGET=(
    [total-and-floor]=40
)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
HOSTILE_EXTRA_ARGS="channels=1 sshloglevel=debug writelimit=98304,131072"

hostile_boot "AxlSsh channel write budget"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"

# --- total-and-floor -------------------------------------------------------------
#
# The total is 131,072 and the floor 819. Connection A opens two channels
# with window 0 -- nothing it is sent can leave -- and runs exec "blob" on
# each, ONE write apiece: X holds 32,768 and Y 98,304 (Y's whole per-channel
# limit), so together they hold the total exactly.
#
# Connection B then, while A holds it all:
#   B2  exec "blob 819": the floor. 131,072 + 819 is past the total, so only
#       the floor admits it -- and it does: accepted, and its bytes arrive.
#   B1  exec "blob 16384": past the floor, onto a full total: refused on the
#       TOTAL with B1's own queue empty, so the only thing that can release
#       it is the total easing (below half, and the write fitting) -- not a
#       drain of its own queue. Nothing B does can ease it.
# Then A's peer adjusts Y's window by 1 MB: Y's 98,304 bytes leave, the total
# falls to X's 32,768, and B1 -- on ANOTHER connection -- must get exactly
# one writable, retry, and deliver all 16,384 bytes.
#
# WHAT MAKES THE WAKE CROSS-CONNECTION. A's peer holds Y open for 4 s after
# reading its CLOSE, before answering with its own. The peer's CLOSE retires
# Y's slot on A's connection, and that path also re-examines every
# connection; so a server that only woke channels of the connection whose
# bytes left would still wake B1 -- late, at A's CLOSE. The check that B1's
# WRITABLE came while A had not yet sent that CLOSE is what tells the two
# apart: a server whose budget wake skips other connections fails it.
if wants total-and-floor; then
    OPEN_Z1=$(hexof chan-open-hex --sender 1 --window 0 --maxpkt 16384)
    ADJ=$(hexof chan-adjust-hex --recipient @ --bytes 1048576)
    EXIT0='peer: got 98 name=exit-status want=0 rest=00000000'
    GOT91_Z='peer: got 91 sender=[0-9]+ window=0 maxpkt=32768'
    m=$(mark)

    (peer budget-a "send:90:$OPEN_Z,expect:91,send:98:$(exec_req 'blob 32768'),expect:99,expect:93,send:90:$OPEN_Z1,expect:91,send:98:$(exec_req 'blob 98304'),expect:99,expect:93,hold:8,send:93:$ADJ,expectsum:98304,expect:98,expect:96,expect:97,hold:4,send:97:$RCPT"; exit "$PEER_RC") &
    tf_a=$!
    BG_PIDS+=("$tf_a")
    check "total-and-floor PRECONDITION: A's two blobs (32768 + 98304) were queued -- the total, exactly" \
        wait_since "$m" 2 20 'SERVER: BLOB-DONE (32768|98304)'

    mb=$(mark)
    (peer budget-b "send:90:$OPEN,expect:91,send:98:$(exec_req 'blob 819'),expect:99,expect:93,expectsum:819,expect:98,expect:96,expect:97,send:97:$RCPT,send:90:$OPEN1,expect:91,send:98:$(exec_req 'blob 16384'),expect:99,expect:93,expectsum:16384,expect:98,expect:96,expect:97,send:97:$RCPT"; exit "$PEER_RC") &
    tf_b=$!
    BG_PIDS+=("$tf_b")
    # B2's verdict, read the moment it lands -- with A's adjust not yet
    # sent, so the total was still full when the floor admitted it.
    tf_floor=none
    if wait_since "$mb" 1 20 'SERVER: BLOB-(DONE|BUSY) 819'; then
        tf_floor=$(since "$mb" | grep -xE 'SERVER: BLOB-(DONE|BUSY) 819' | head -1 | awk '{print $2}' || true)
    fi
    tf_adjust_at_floor=$(grep -c 'peer: sent 93' "$TEST_TMPDIR/ssh-budget-a.log" || true)
    tf_busy=0
    if wait_since "$mb" 1 20 'SERVER: BLOB-BUSY 16384'; then tf_busy=1; fi
    sleep 0.5
    # Sampled while A still holds Y: its adjust not yet in its transcript.
    tf_writable_pre=$(since_lines "$m" 'SERVER: WRITABLE')
    tf_adjust_sent=$(grep -c 'peer: sent 93' "$TEST_TMPDIR/ssh-budget-a.log" || true)

    # The wake, and whether A had answered Y's CLOSE by then.
    tf_woken=0
    if wait_since "$m" 1 30 'SERVER: WRITABLE'; then tf_woken=1; fi
    tf_a_closed=$(grep -c 'peer: sent 97' "$TEST_TMPDIR/ssh-budget-a.log" || true)

    tf_a_rc=0; wait "$tf_a" || tf_a_rc=$?
    tf_b_rc=0; wait "$tf_b" || tf_b_rc=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-budget-a.log" "$TEST_TMPDIR/ssh-budget-b.log")
    settle "$m" 2 total-and-floor

    check "total-and-floor PRECONDITION: B1's 16384 was refused while A held the total" \
        test "$tf_busy" = 1
    check "total-and-floor PRECONDITION: the samples were taken before A's adjust was sent" \
        test "${tf_adjust_sent:-0}" = 0
    check "total-and-floor: the floor -- B2's 819 bytes accepted onto a full total (A's adjust not yet sent)" \
        test "$tf_floor" = BLOB-DONE -a "${tf_adjust_at_floor:-0}" = 0
    check "total-and-floor: B1 refused once, on the total, and no writable while A held it" \
        test "$(since_lines "$mb" 'SERVER: BLOB-BUSY 16384')" = 1 \
            -a "$(since_re "$m" 'SERVER: BLOB-BUSY [0-9]+')" = 1 \
            -a "$tf_writable_pre" = 0
    check "total-and-floor: A's bytes leaving woke B1 on the OTHER connection, before A's CLOSE" \
        test "$tf_woken" = 1 -a "${tf_a_closed:-0}" = 0
    check "total-and-floor: exactly one writable, and B1's retry was accepted whole" \
        test "$(since_lines "$m" 'SERVER: WRITABLE')" = 1 \
            -a "$(since_lines "$mb" 'SERVER: BLOB-DONE 16384')" = 1
    PEER_RC=$tf_a_rc
    check "total-and-floor: A -- two windowless blobs, then after its adjust all of Y, exit 0, EOF, CLOSE" \
        transcript budget-a 0 "$OPENED" \
            "$(grep -xE "$GOT91_Z" "$TEST_TMPDIR/ssh-budget-a.log" | sed -n 1p || true)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 90 seq=4' \
            "$(grep -xE "$GOT91_Z" "$TEST_TMPDIR/ssh-budget-a.log" | sed -n 2p || true)" \
            'peer: sent 98 seq=5' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 93 seq=6' \
            "peer: sum out=98304 err=0 sha256=$(nsum b 98304)" \
            "$EXIT0" 'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=7'
    PEER_RC=$tf_b_rc
    check "total-and-floor: B -- the floor blob at once, then the refused blob whole after the wake" \
        transcript budget-b 0 "$OPENED" \
            "$(grep -xE "$GOT91" "$TEST_TMPDIR/ssh-budget-b.log" | sed -n 1p || true)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            "peer: sum out=819 err=0 sha256=$(nsum b 819)" \
            "$EXIT0" 'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=4' \
            'peer: sent 90 seq=5' \
            "$(grep -xE "$GOT91" "$TEST_TMPDIR/ssh-budget-b.log" | sed -n 2p || true)" \
            'peer: sent 98 seq=6' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            "peer: sum out=16384 err=0 sha256=$(nsum b 16384)" \
            "$EXIT0" 'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=7'
fi

if [[ "$ABUSE" == 1 ]]; then
    check "every scenario: no channel hook nested inside another of its channel's" \
        test "$(since_lines 0 'SERVER: HOOK-NESTED')" = 0
    check "every scenario: no fixture write or exit failed" \
        test "$(since_re 0 'SERVER: (WRITE|EXIT)-FAIL -?[0-9]+')" = 0
fi

hostile_finish
