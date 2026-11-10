# shellcheck shell=bash
# ssh-channel-lib.sh -- the helpers shared by the AxlSsh channel suites that
# drive the fixture shell (ssh-server-selftest.c, `channels=1`) at the byte
# level with ssh-peer.py: test-ssh-channel-hostile-qemu.sh,
# test-ssh-channel-budget-qemu.sh, test-ssh-rekey-hostile-qemu.sh and the two
# idle suites. Sourced AFTER lib/ssh-hostile-lib.sh's
# hostile_boot -- the message bodies below are built by ssh-peer.py when this
# is sourced, and every helper reads that file's state (TEST_CLEAN_LOG,
# HOST_PORT, PW_OK, check, peer_is, DUMP_LOGS). Never run on its own.

PEER="$(dirname "$0")/ssh-peer.py"

# The guest log's line count now, and the lines after a mark; how many of
# those are EXACTLY $2 / match the ERE $2 whole; and a wait of up to $3 s
# for at least $2 of them matching ERE $4.
mark() { test_clean_log; wc -l < "$TEST_CLEAN_LOG" | tr -d ' '; }
since() { test_clean_log; sed -n "$(($1 + 1)),\$p" "$TEST_CLEAN_LOG"; }
since_lines() { local n; n=$(since "$1" | grep -cxF -- "$2" || true); echo "${n:-0}"; }
since_re()    { local n; n=$(since "$1" | grep -cxE -- "$2" || true); echo "${n:-0}"; }
wait_since() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        if [[ "$(since_re "$1" "$4")" -ge "$2" ]]; then return 0; fi
        sleep 0.2
    done
    return 1
}

# settle MARK CONNS LABEL -- as a CHECK, wait (up to 20 s) until CONNS
# connections have finished their teardown after mark MARK: CONNS lines of
# `SERVER: PRINCIPAL-RELEASED`. conn_free prints it after chan_close_all has
# run every close hook AND the server has written what follows each hook
# (`chan N: closed`, and, for a started session freed by the peer's CLOSE,
# the unused-clock restart line), so it is the last line a connection
# prints. The close hook (`SERVER: CHANNEL-CLOSE`) is NOT: waiting for it
# returns with those lines still in flight on the serial line, and the next
# scenario's mark then counts them as its own (measured under host load, and
# reproduced by delaying the fixture's close hook). A check that some line is
# ABSENT, or that counts lines, runs after this: the serial log is in order,
# so a line still in flight cannot be missed.
# CONNS is the number of LOGINS (not channels) the scenario made; one short
# is satisfied by the previous scenario's own late release, which is this
# race again one line later. A teardown that never comes is a failure here,
# not a silent wait -- an absence checked without it would not know the
# question had been asked.
settle() {
    check "$3: $2 connection(s) finished their teardown (PRINCIPAL-RELEASED)" \
        wait_since "$1" "$2" 20 'SERVER: PRINCIPAL-RELEASED'
}

# One ssh-peer.py helper's output; a helper that fails aborts the test (a
# usage error here is the test's own bug, never the server's).
hexof() {
    local out
    out=$(python3 "$PEER" "$@") || { echo "FAIL: ssh-peer.py $* failed"; exit 1; }
    echo "$out"
}

# peer NAME SCRIPT [login args...] -- one scripted login, as pwuser unless
# PEER_USER and PEER_PW name another staged credential. The transcript goes
# to ssh-NAME.log: hostile_finish counts its `peer: authenticated` line as a
# login that owes one principal release. Sets PEER_RC.
peer() {
    local name="$1" script="$2"
    shift 2
    PEER_RC=0
    timeout 90 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" \
        --user "${PEER_USER:-pwuser}" --password "${PEER_PW:-$PW_OK}" \
        --script "$script" "$@" \
        > "$TEST_TMPDIR/ssh-$name.log" 2>&1 || PEER_RC=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$name.log")
}

# The transcript of peer NAME is exactly the lines given, after its
# `peer: authenticated`, and it exited RC.
transcript() {
    local name="$1" rc="$2"
    shift 2
    peer_is "$PEER_RC" "$rc" "$TEST_TMPDIR/ssh-$name.log" \
        'peer: authenticated' "$@"
}

# sha256 of N bytes of the character C, optionally after a prefix -- what
# `expectsum` reports for N bytes of C, and what the fixture's `sink`
# reports for a `pour` of them.
nsum() { { printf '%s' "${3:-}"; head -c "$2" /dev/zero | tr '\0' "$1"; } | sha256sum | awk '{print $1}'; }

# ONE spelling of every message body these scenarios send. @ = the channel
# the peer last saw confirmed; RCPT is a recipient-only body (EOF, CLOSE).
RCPT='@00000000'
OPEN=$(hexof chan-open-hex)
OPEN1=$(hexof chan-open-hex --sender 1)
OPEN_Z=$(hexof chan-open-hex --window 0 --maxpkt 16384)
SHELL_REQ=$(hexof chan-req-hex --recipient @ --name shell --want 1)
u32() { printf '%08x' "$1"; }
str() { printf '%08x' "${#1}"; printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }
exec_req() { hexof chan-req-hex --recipient @ --name exec --want 1 --rest "$(str "$1")"; }
data() { hexof chan-data-hex --recipient @ --text "$1"; }
KEEP='global:keepalive@openssh.com:1,expect:82'
OPENED='peer: sent 90 seq=2'
GOT91='peer: got 91 sender=[0-9]+ window=0 maxpkt=32768'

# The confirmation's sender is the server's number for the channel; the
# transcript holds whatever it was. Pinned to its shape, and the window
# (0 until the session starts) and maximum packet exactly.
got91() { grep -xE "$GOT91" "$TEST_TMPDIR/ssh-$1.log" | head -1 || true; }
