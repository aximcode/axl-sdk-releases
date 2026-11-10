#!/bin/bash
# test-meta: arch=both needs=ssh est=60 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-auth-qemu.sh — AxlSsh userauth (publickey and password), graded by
# a real ssh(1).
#
# WHY ssh(1): the same argument as the transport test. A signature scheme we
# implement self-consistently verifies against itself and against nothing
# else; RFC 4252 section 7's signed blob has six length-prefixed fields and
# a session id, and being wrong in any of them still round-trips through our
# own code. OpenSSH does not agree with a blob we got wrong.
#
# THE THIRD ASSERTION IS THE POINT. Spec section 9 originally said any failed
# auth attempt costs the connection. `ssh -i bad -i good` is what proves that
# reading was wrong and the amended one is right: OpenSSH offers candidate
# keys in sequence on ONE connection and never reconnects, so a server that
# drops on the first miss cannot log in a user whose good key is second.
#
# NO KEY MATERIAL IS COMMITTED. Both pairs are generated per run into
# $TEST_TMPDIR and only the public half of one reaches the guest.
#
# Usage: ./test/integration/test-ssh-auth-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/ssh-guest-lib.sh"

test_parse_args "$@"

if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: no ssh(1)/ssh-keygen on the host — OpenSSH is this test's oracle"
    exit 77
fi

test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=2222
SERVER_DEADLINE_MS=90000

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all ssh-server-selftest 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/ssh-server-selftest.efi"

ssh-keygen -q -t ed25519 -N '' -C 'axl-p2-good' -f "$TEST_TMPDIR/good" </dev/null
ssh-keygen -q -t ed25519 -N '' -C 'axl-p2-bad'  -f "$TEST_TMPDIR/bad"  </dev/null

{
    echo "# generated per run by test-ssh-auth-qemu.sh -- never committed"
    echo ""
    cat "$TEST_TMPDIR/good.pub"
} > "$TEST_STAGING/authorized_keys"

# Passwords are generated per run and never committed. Hex from /dev/urandom:
# no ':' (the fixture's field separator) and no shell metacharacters.
rand_hex() { od -An -tx1 -N8 /dev/urandom | tr -d ' \n'; }
PW_OK=$(rand_hex)
PW_CHANGE=$(rand_hex)
PW_SLOW=$(rand_hex)
PW_WRONG=$(rand_hex)
{
    echo "pwuser:${PW_OK}:ok"
    echo "changeme:${PW_CHANGE}:change"
    echo "slowuser:${PW_SLOW}:slow"
} > "$TEST_STAGING/credentials"

# ssh(1) reads the password from SSH_ASKPASS when SSH_ASKPASS_REQUIRE=force,
# regardless of DISPLAY. Each invocation appends one line to a counter file:
# the count of password PROMPTS is the most direct evidence of how many
# attempts the server allowed.
#
# ASKPASS_EMPTY_FIRST=1 answers the FIRST prompt with an empty password and
# every later one with $ASKPASS_PASSWORD. That is how a real ssh(1) sends a
# zero-length password -- sshconnect2.c's userauth_passwd does not check what
# read_passphrase returned, so "" goes on the wire as an ordinary password
# request. It is what proves axl_ssh_password_reaches_hook refuses it BEFORE
# the application hook, which no client-side observation could show.
cat > "$TEST_TMPDIR/askpass.sh" <<'ASKPASS'
#!/bin/sh
echo x >> "$ASKPASS_COUNT"
n=$(wc -l < "$ASKPASS_COUNT")
if [ "${ASKPASS_EMPTY_FIRST:-0}" = 1 ] && [ "$n" = 1 ]; then
    printf '\n'
else
    printf '%s\n' "$ASKPASS_PASSWORD"
fi
ASKPASS
chmod +x "$TEST_TMPDIR/askpass.sh"

# sshloglevel=debug for the whole boot: the transmit checks count the
# server's per-send DEBUG line ("tx: F frames, B bytes"), and at the library's
# default level a DEBUG line is not demoted in the guest log, it is ABSENT.
# Nothing else here is loosened by it -- measured: every check below passes
# at both levels, and the audit-log count further down matches "ssh: Failed",
# which the rate limiter's DEBUG copies (prefixed "ssh:log_audit:<line>:")
# do not.
# The startup commands live in test-ssh-auth-qemu.nsh; it names the staged
# files relatively (booted: fs0:\, pooled: the lease's share directory).
test_use_nsh "$GUEST_PORT" "$SERVER_DEADLINE_MS"

test_build_image

echo "=== AxlSsh userauth (publickey + password) vs $(ssh -V 2>&1) ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
trap 'test_cleanup' EXIT

echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT"

if ! test_wait_boot "ssh: listening" 90; then
    echo "FAIL: SSH server did not start listening within ${TEST_WAITED_S} seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server is listening"

# common-test.sh installs `set -euo pipefail`, so a bare `grep -q` followed by
# `check "$?"` never reaches check -- the failing grep kills the script and
# the run reports no FAIL line at all. The grep therefore goes INSIDE the
# condition, where a non-zero status is a value rather than a fatal error.
pass=0; fail=0
check() {
    local name="$1"; shift
    if "$@"; then
        echo "PASS: $name"; pass=$((pass+1))
    else
        echo "FAIL: $name"; fail=$((fail+1))
    fi
}

# Every run is expected to end non-zero: authentication succeeds and then the
# channel is refused, because session channels are P4. The transcript is what
# is graded, never the exit status.
#
# Destination is "${SSH_USER:-root}@127.0.0.1": a user@host destination takes
# precedence over -l, so this is what lets a caller pick the login name (the
# fixture's publickey hook denies exactly one name, "nokey") while every
# existing call -- which sets no SSH_USER -- keeps logging in as root.
run_ssh() {
    local log="$1"; shift
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o BatchMode=yes \
        -o ConnectTimeout=20 \
        -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey \
        "$@" "${SSH_USER:-root}@127.0.0.1" true > "$log" 2>&1 || true
}

# NOT BatchMode: BatchMode disables password prompting outright, so every
# password scenario would "fail to authenticate" for the client's reason, not
# the server's.
#
# NumberOfPasswordPrompts=5, and this is load-bearing: ssh's default is 3,
# which equals the server's cap. With the default, a server with NO cap would
# still show exactly 3 prompts, because ssh stops on its own. At 5, three
# prompts can only mean the server ended the connection.
run_ssh_pw() {
    local log="$1" user="$2" pass="$3"
    shift 3
    : > "$log.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_EMPTY_FIRST="${ASKPASS_EMPTY_FIRST:-0}" \
    ASKPASS_PASSWORD="$pass" ASKPASS_COUNT="$log.count" \
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 \
        -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        -o NumberOfPasswordPrompts=5 \
        "$@" "$user@127.0.0.1" true > "$log" 2>&1 < /dev/null || true
}
prompts() { wc -l < "$1.count" | tr -d ' '; }

# ssh-peer.py (added in P4a): the scripted peer later P4 tests are built on.
# It must prove it can SEE: its own selftest carries a failing control, a
# good password must authenticate, and a wrong one must be refused -- a peer
# that "authenticates" whatever it sends would pass every later test for the
# wrong reason. `kexwait` is proven here too, before any test relies on it:
# key exchange completes and the server keeps holding the connection.
#
# FIRST, AND FOLLOWED BY A SLEEP, because of the penalty note further down
# (read it). The wrong password is a failing connection (authfail, 5 s) and
# the kexwait holder one that never tried to authenticate (noauth, 1 s); a
# good login accrues nothing. Run anywhere later they would push this file
# over the shipped threshold and get a LATER scenario refused at accept.
# Here the table is empty, and it can be emptied again deterministically:
# the wrong password accrues 5 at t0, kexwait closes no earlier than t0 + 3
# (its hold) and adds 1, so what remains is at most 5 - 3 + 1 = 3 s, burning
# down in real time. `sleep 4` covers it with 1 s for the server noticing the
# close after the peer has exited. Everything after starts from a clean
# table, so the arithmetic in that note is unchanged.
#
# Every capture tolerates a non-zero exit (common-test.sh runs `set -e`), and
# the wrong password's exit status is graded on its own: 2 is `auth failed`,
# and a peer that exited for any other reason printed something else.
PEER="$TESTS_DIR/ssh-peer.py"
check "peer: selftest vectors and control" \
    test "$(python3 "$PEER" selftest)" = "selftest: ok"
PEER_OK=$(timeout 60 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" \
              --user pwuser --password "$PW_OK" 2>&1) || true
check "peer: a good password authenticates" \
    test "$PEER_OK" = "peer: authenticated"
# `burst:` and `expectn:` are proven here before a later test asserts on
# them: fifty pre-sealed CHANNEL_OPENs leave in ONE sendall and must draw
# fifty CHANNEL_OPEN_FAILUREs (type 92). The channel type is `x11`, which
# AxlSsh refuses in every phase (spec section 12a.3), so this reply cannot
# change under the test when `session` opens. The body is RFC 4254 section
# 6.3.2's whole x11 open: string "x11", sender channel 1, initial window
# 32768, max packet 32768, originator "127.0.0.1", port 6010 -- 36 bytes,
# 00000003 783131 00000001 00008000 00008000 00000009 3132372e302e302e31
# 0000177a -- and it is PRINTED by the peer (`x11-open-hex`, the same
# encoder bench uses) rather than typed here. The originator fields matter:
# without them the open is malformed, and a server that ended the connection
# on every WELL-FORMED non-session open passed this burst. seqs=2..51
# because strict KEX restarts the peer's sequence at NEWKEYS: SERVICE_REQUEST
# is 0 and USERAUTH_REQUEST 1 -- measured against this tree.
#
# THE CONTROL is the second login: `expectn` told to expect REQUEST_FAILURE
# (82) must refuse the 92s it actually gets. Without it an `expectn` that
# accepted any type would pass the first check, because the server really
# does send 92. Both logins authenticate, so neither accrues a penalty or
# moves the arithmetic in the note below.
#
# THE HEX IS CHECKED BEFORE IT IS USED. A failed helper prints nothing, and
# bytes.fromhex("") is b'', so an unchecked empty value sends a CHANNEL_OPEN
# with no body at all -- which the checks below would then report as a
# server fault. 72 hex characters is the 36-byte body above, exactly.
X11_OPEN_HEX_RC=0
X11_OPEN_HEX=$(python3 "$PEER" x11-open-hex) || X11_OPEN_HEX_RC=$?
if [[ "$X11_OPEN_HEX_RC" -ne 0 || ! "$X11_OPEN_HEX" =~ ^[0-9a-f]{72}$ ]]; then
    echo "FAIL: ssh-peer.py x11-open-hex: rc=$X11_OPEN_HEX_RC, printed '$X11_OPEN_HEX' -- want 72 hex characters"
    exit 1
fi

# ONE TCP SEND CARRIES MANY FRAMES (the transmit queue). The server writes
# "tx: F frames, B bytes" at DEBUG once per axl_tcp_send_async; the burst's
# slice of the guest log is everything between these two marks. Serial output
# is written before the send it describes, so by the time the peer has read
# its replies the lines are in the log -- and the next connection starts after
# the second mark. See the checks after the ssh(1) runs.
TX_MARK_BURST0=$(wc -l < "$TEST_LOG")
PEER_BURST_RC=0
PEER_BURST=$(timeout 60 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" \
                 --user pwuser --password "$PW_OK" \
                 --script "burst:50:90:$X11_OPEN_HEX,expectn:50:92" 2>&1) \
    || PEER_BURST_RC=$?
TX_MARK_BURST1=$(wc -l < "$TEST_LOG")
check "peer: a 50-packet burst draws 50 CHANNEL_OPEN_FAILUREs (rc=$PEER_BURST_RC)" \
    test "$PEER_BURST" = $'peer: authenticated\npeer: sent burst 50 x 90 seqs=2..51\npeer: got 50 x 92' \
         -a "$PEER_BURST_RC" = 0
PEER_NOTN_RC=0
PEER_NOTN=$(timeout 60 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" \
                --user pwuser --password "$PW_OK" \
                --script "burst:2:90:$X11_OPEN_HEX,expectn:2:82" 2>&1) \
    || PEER_NOTN_RC=$?
check "peer: expectn CONTROL refuses a type it was not told to expect (rc=$PEER_NOTN_RC)" \
    test "$PEER_NOTN" = $'peer: authenticated\npeer: sent burst 2 x 90 seqs=2..3\npeer: unexpected 92' \
         -a "$PEER_NOTN_RC" = 3
PEER_BAD_RC=0
PEER_BAD=$(timeout 60 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" \
               --user pwuser --password "$PW_WRONG" 2>&1) || PEER_BAD_RC=$?
check "peer: a wrong password is refused (rc=$PEER_BAD_RC)" \
    test "$PEER_BAD" = "peer: auth failed" -a "$PEER_BAD_RC" = 2
PEER_KEX_RC=0
PEER_KEX=$(timeout 60 python3 "$PEER" kexwait 127.0.0.1 "$HOST_PORT" \
               --hold 3 2>&1) || PEER_KEX_RC=$?
check "peer: kexwait completes key exchange and is held (rc=$PEER_KEX_RC)" \
    test "$PEER_KEX" = $'peer: kex complete\npeer: held rx=0' -a "$PEER_KEX_RC" = 0
sleep 4

GOOD_LOG="$TEST_TMPDIR/ssh-good.log"
BAD_LOG="$TEST_TMPDIR/ssh-bad.log"
SEQ_LOG="$TEST_TMPDIR/ssh-seq.log"

TX_MARK_GOOD0=$(wc -l < "$TEST_LOG")
run_ssh "$GOOD_LOG" -i "$TEST_TMPDIR/good"
TX_MARK_GOOD1=$(wc -l < "$TEST_LOG")
run_ssh "$BAD_LOG"  -i "$TEST_TMPDIR/bad"
run_ssh "$SEQ_LOG"  -i "$TEST_TMPDIR/bad" -i "$TEST_TMPDIR/good"

# RFC 4254 section 4's GLOBAL_REQUEST, answered with REQUEST_FAILURE (P4a),
# driven by a real client two ways.
#
# `-R` makes ssh(1) send SSH2_MSG_GLOBAL_REQUEST "tcpip-forward" (type 80)
# right after authenticating. Port 0 asks the server to choose, and
# 127.0.0.1:9 (discard) is the destination nothing will ever connect to; the
# forward is refused -- AxlSsh opens none -- and ExitOnForwardFailure is off
# by default, so the session carries on to the channel open exactly as the
# other runs do.
#
# The second is `keepalive@openssh.com` (ServerAliveInterval), the liveness
# probe sshd itself answers with REQUEST_FAILURE: "the reply IS the liveness
# signal" (spec section 12a). See the keepalive checks below for why this one
# is a regression guard rather than a discriminator.
#
# Both connections AUTHENTICATE, which is why it is safe to add them here:
# the penalty note below turns on the count of connections that FAIL, and an
# authenticated one accrues nothing.
UNIMPL_LOG="$TEST_TMPDIR/ssh-unimpl.log"
run_ssh "$UNIMPL_LOG" -i "$TEST_TMPDIR/good" -R 127.0.0.1:0:127.0.0.1:9

KEEPALIVE_LOG="$TEST_TMPDIR/ssh-keepalive.log"
KEEPALIVE_RC=0
timeout 8 ssh -vvv -p "$HOST_PORT" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes \
    -o ConnectTimeout=20 \
    -o IdentitiesOnly=yes \
    -o PreferredAuthentications=publickey \
    -o ServerAliveInterval=1 \
    -o ServerAliveCountMax=2 \
    -N -i "$TEST_TMPDIR/good" "${SSH_USER:-root}@127.0.0.1" \
    > "$KEEPALIVE_LOG" 2>&1 || KEEPALIVE_RC=$?

in_good()  { grep -q "$1" "$GOOD_LOG"; }
in_bad()   { grep -q "$1" "$BAD_LOG"; }
in_seq()   { grep -q "$1" "$SEQ_LOG"; }

# ONE definition, deliberately. This exact string is what proves a login
# succeeded AND -- negated -- what proves the wrong key did not get in. The
# brief's original spelling ("Authentication succeeded (publickey)") does not
# appear at all under OpenSSH 9.9p1, which made the negative check a
# tautology that passed no matter what the server did. Written out three
# times, the next version bump reintroduces that bug in whichever copy the
# fixer misses. Update it HERE and every use moves together.
#
# AND THERE IS A SECOND COPY, in another file: lib/ssh-hostile-lib.sh
# defines the same SUCCESS_RE for the hostile tests' liveness logins (both
# halves source it). An OpenSSH bump has to move BOTH. Deliberately
# duplicated rather than shared -- the hostile halves share a helper because
# they are one suite split in two for time, but this file is independent of
# them, and a sourced fragment between independent test scripts would be
# worse than a documented duplicate -- so this pointer IS the safeguard.
#
# OpenSSH 9.9p1 (this repo's tested version) does NOT print "Authentication
# succeeded (publickey)" -- checked directly against every transcript below,
# that string appears in NEITHER the good nor the bad log, in any run. What
# 9.9p1 actually prints on success is the summary line
# `Authenticated to <host> ([<host>]:<port>) using "publickey".`
SUCCESS_RE='Authenticated to .* using "publickey"'

# The query form: ssh offered the key and we answered PK_OK. Without this
# line ssh would never have signed anything, so it grades the half of RFC 4252
# section 7 that runs no crypto.
check "query form: ssh(1) reports the server accepted its key" \
    in_good "Server accepts key"

# The signed form: ssh built the section 7 blob, signed it, and we verified
# it against the 32 bytes out of authorized_keys.
check "signed form: authentication succeeded by public key" \
    in_good "$SUCCESS_RE"

# The post-auth boundary: this boot installs no channel hooks, so a
# successful login's session open is refused as UNKNOWN_CHANNEL_TYPE, exactly
# as before session channels existed -- the opt-in axl-ssh-channel.h
# promises. The WHOLE line ssh(1) prints for our reason code (the
# description is empty, so ssh ends it with ": "), measured from a real
# transcript; CRLF-terminated like every ssh(1) line in this file.
OPEN_REFUSED_RE='^channel 0: open failed: unknown channel type: [[:space:]]*$'
check "post-auth boundary: the session channel is refused cleanly" \
    grep -qE "$OPEN_REFUSED_RE" "$GOOD_LOG"

# An unauthorized key is refused.
check "wrong key: refused" \
    in_bad "Permission denied"

bad_never_authenticated() {
    ! grep -q "$SUCCESS_RE" "$BAD_LOG"
}
check "wrong key: never authenticated" bad_never_authenticated

# THE AMENDMENT. A server that drops the connection on the first unauthorized
# key cannot reach the second one, so this line fails against the literal
# reading of the original spec section 9 and passes against the amended one.
check "key list: ssh offering bad then good still authenticates" \
    in_seq "$SUCCESS_RE"

# NOT a server assertion -- this is a line ssh(1) prints when it decides to
# offer an identity, before anything is sent, and no server behaviour can
# change it. It is here to guard the PREMISE of the check above: that test
# only exercises the spec section 9 amendment if the bad key really is
# offered first. If a future OpenSSH reorders identities, this fails and
# tells us the amendment check has gone hollow -- which is the only way we
# would ever find out.
#
# OpenSSH 9.9p1 (this repo's tested version) logs the key FILE PATH here, not
# the -C comment -- `debug1: Offering public key: /tmp/.../bad ED25519
# SHA256:... explicit` -- the opposite of the brief's "newer versions log the
# comment" guess, checked directly against this transcript. The comment never
# appears on this line at all. The path is what discriminates instead: both
# keys are generated with -f "$TEST_TMPDIR/{good,bad}", so the path always
# ends in literal "/bad" for the bad key and "/good" for the good one -- the
# trailing "/bad " (before " ED25519") cannot match the good key's line.
check "key list PRECONDITION: ssh offered the bad key first" \
    in_seq "Offering public key: .*/bad ED25519"

# (a) `-R` draws a real RFC 4254 section 4 REQUEST_FAILURE, not the
# UNIMPLEMENTED stand-in P1 shipped. ssh(1) prints this exact line once its
# global-request callback runs, which only happens on an actual
# REQUEST_FAILURE (or REQUEST_SUCCESS) reply -- measured against this tree.
# Measured RED: the pre-P4a server DOES reply to GLOBAL_REQUEST -- with the
# generic post-auth UNIMPLEMENTED stand-in, same as any other message it does
# not implement -- but an UNIMPLEMENTED reply never reaches the pending
# global-request callback, so it prints "debug1: Received
# SSH2_MSG_UNIMPLEMENTED for 4" and nothing else: this exact line is absent
# from that transcript.
#
# Anchored with `[[:space:]]*$`, not matched as a plain substring: like every
# other ssh(1) line in this file, it is CRLF-terminated (confirmed with
# `cat -A` against a captured transcript), so a literal exact-string match
# against the line without its trailing `\r` never matches at all.
unimpl() { grep -q "$1" "$UNIMPL_LOG"; }
check "GLOBAL_REQUEST: -R draws a real REQUEST_FAILURE" \
    grep -qE '^Warning: remote port forwarding failed for listen port 0[[:space:]]*$' "$UNIMPL_LOG"

# ...and the session survived it: the reply is a diagnostic, not a teardown.
# Without this the check above would pass just as well against a server that
# answered and then dropped the connection.
check "GLOBAL_REQUEST: the session carries on to the channel open" \
    grep -qE "$OPEN_REFUSED_RE" "$UNIMPL_LOG"

# One reply for the whole session, and it must be THIS reply: nothing
# AxlSsh handles should still fall through to the section 11.4 UNIMPLEMENTED
# stand-in now that GLOBAL_REQUEST has its own answer, so a regression back
# to answering it with UNIMPLEMENTED again shows up here as a count above 0
# (it was exactly 1 before this task; P4a retires that reply for this
# message, so the right count is now 0).
#
# IT DOES NOT COVER THE SSH_MSG_DISCONNECT EXCLUSION, and saying so is the
# point of this paragraph. ssh sends type 1 as the very last thing it does
# and closes immediately, so a reply to it is never read and never logged:
# removing DISCONNECT from axl_ssh_unimplemented_due's exclusion list was
# sabotaged here and this file went 26 passed / 0 failed (against the
# pre-P4a version of this check, which counted UNIMPLEMENTED replies rather
# than their absence -- the blind spot itself is unchanged by that rewrite).
# The assertion that DOES catch it is the unit one, "ssh unimplemented:
# DISCONNECT is never answered" (test/unit/axl-test-ssh.c), verified by the
# same sabotage. Do not widen this check to claim the DISCONNECT case; from
# a client there is nothing to see.
n_unimpl=$(grep -c "Received SSH2_MSG_UNIMPLEMENTED for" "$UNIMPL_LOG" || true)
check "GLOBAL_REQUEST: the -R session draws no UNIMPLEMENTED (got ${n_unimpl})" \
    test "${n_unimpl:-0}" = 0

good_drew_no_unimplemented() {
    ! grep -q "Received SSH2_MSG_UNIMPLEMENTED for" "$GOOD_LOG"
}
check "UNIMPLEMENTED: an ordinary session draws none at all" \
    good_drew_no_unimplemented

# (b) Keepalive survives, and draws the right reply. sshd answers
# keepalive@openssh.com the same way -- REQUEST_FAILURE is the liveness
# signal (spec section 12a) -- so a client polling every second for up to
# 8 seconds must still be connected when `timeout` ends it (rc 124), never
# torn down by the client's own liveness logic.
check "keepalive: still connected when the client's own timeout ends it (rc=$KEEPALIVE_RC)" \
    test "$KEEPALIVE_RC" = 124

# THIS PAIR IS A REAL DISCRIMINATOR, unlike the rc/timeout check above (the
# pre-P4a tree also exits 124 there -- see the next check's comment for why).
# `in_keepalive` reuses the SAME $SUCCESS_RE the GOOD_LOG checks above use --
# one definition -- to prove this connection authenticated at all, and the
# type-82 count is the reply itself: at least two REQUEST_FAILURE packets
# (measured well above 2 against this tree: ServerAliveInterval=1 against an
# 8 s timeout draws one about every second). Against the pre-P4a tree this
# count is 0 -- its equivalent line reads "debug3: receive packet: type 3",
# never "type 82" -- confirmed by sabotage (see the commit's fix report).
in_keepalive() { grep -q "$1" "$KEEPALIVE_LOG"; }
check "keepalive: authenticated" \
    in_keepalive "$SUCCESS_RE"
n_keepalive_82=$(grep -c "receive packet: type 82" "$KEEPALIVE_LOG" || true)
check "keepalive: at least two REQUEST_FAILURE replies (got ${n_keepalive_82})" \
    test "${n_keepalive_82:-0}" -ge 2

# NOT A DISCRIMINATOR ON ITS OWN -- measured, not assumed: run against the
# pre-P4a tree too, this ALSO exits 124 with no "not responding" line.
# OpenSSH's liveness check is satisfied by ANY reply to the pending global
# request, including the UNIMPLEMENTED stand-in P1 shipped, not only
# REQUEST_FAILURE -- which is exactly why the type-82 count above, not this
# negative, is what actually discriminates. Kept as a regression guard
# against the one thing that WOULD break it: going back to silence.
keepalive_never_reported_unresponsive() {
    ! grep -qE 'Timeout, server .* not responding' "$KEEPALIVE_LOG"
}
check "keepalive: the client never reports the server unresponsive" \
    keepalive_never_reported_unresponsive

# Same regression guard as the -R session above: a keepalive session must
# never fall through to the UNIMPLEMENTED stand-in either.
n_unimpl_keepalive=$(grep -c "Received SSH2_MSG_UNIMPLEMENTED for" "$KEEPALIVE_LOG" || true)
check "GLOBAL_REQUEST: the keepalive session draws no UNIMPLEMENTED (got ${n_unimpl_keepalive})" \
    test "${n_unimpl_keepalive:-0}" = 0

# (c) SSH_MSG_UNIMPLEMENTED (RFC 4253 section 11.4), and both halves of
# GLOBAL_REQUEST's want_reply, driven by ssh-peer.py (added in P4a) rather than
# ssh(1): ssh(1) has no way to send an arbitrary unrecognised message or a
# GLOBAL_REQUEST with want_reply FALSE, so `-R` above is the only real-client
# route to either, and it now draws REQUEST_FAILURE rather than
# UNIMPLEMENTED. The script sends two probes ssh-peer.py can address
# directly (200, 201 -- unrecognised by construction) bracketing two global
# requests, one of each want_reply value.
UNIMPL_OUT=$(timeout 60 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" \
    --user pwuser --password "$PW_OK" \
    --script 'send:200,expect:3,global:x@example.org:0,send:201,expect:3,global:keepalive@openssh.com:1,expect:82' 2>&1) || true
# Expected output, one line per action -- measured against this tree:
#   1 peer: authenticated
#   2 peer: sent 200 seq=2
#   3 peer: got 3 seq=2
#   4 peer: sent 80 seq=3
#   5 peer: sent 201 seq=4
#   6 peer: got 3 seq=4
#   7 peer: sent 80 seq=5
#   8 peer: got 82
# With strict KEX the peer's sequence restarts at 0 after NEWKEYS:
# SERVICE_REQUEST is 0, USERAUTH_REQUEST "password" 1, so the first probe is 2.
check "UNIMPLEMENTED: the reply echoes the probe's sequence number" \
    test "$(sed -n 3p <<<"$UNIMPL_OUT")" = "peer: got 3 seq=2"
# want_reply FALSE draws NOTHING: the next message after the silent global
# request must be the UNIMPLEMENTED for the probe that follows it (seq 4),
# not a REQUEST_FAILURE and not an UNIMPLEMENTED for the global request
# itself. Measured RED: on the pre-P4a tree this line reads "peer: got 3
# seq=3" instead -- the old code answered every GLOBAL_REQUEST with
# UNIMPLEMENTED regardless of want_reply, so the silent request drew a reply
# of its own and every later sequence number is one lower.
check "GLOBAL_REQUEST want_reply 0 is answered by silence" \
    test "$(sed -n 6p <<<"$UNIMPL_OUT")" = "peer: got 3 seq=4"
# Measured RED: on the pre-P4a tree this line reads "peer: unexpected 3" (the
# peer's expect:82 action raises PeerExit, rc 3) -- want_reply TRUE drew the
# same UNIMPLEMENTED stand-in as everything else, never a REQUEST_FAILURE.
check "GLOBAL_REQUEST want_reply 1 draws REQUEST_FAILURE" \
    test "$(sed -n 8p <<<"$UNIMPL_OUT")" = "peer: got 82"

# THIS FILE SITS EXACTLY ON THE SHIPPED PENALTY THRESHOLD -- read before
# adding a connection. The fixture runs the SHIPPED per-source penalties
# (axl_ssh_server_set_penalties: authfail 5 s, min 15 s, refused only once a
# source's penalty is OVER 15), and every connection here comes from one
# address. (ssh-peer.py's wrong password and kexwait holder, at the very top,
# are burned off by the sleep after them and are not counted here.) Four of
# them fail authentication: BAD_LOG above, then pw-bad,
# pw-change and pk-nokey below. The first three reach at most 3 x 5 = 15 s,
# which is not over 15, so pk-nokey is still served -- and it is the LAST
# connection, so its own accrual refuses nothing. Add another failing
# connection anywhere before pk-nokey, or any connection after it, and that
# connection is REFUSED at accept: it fails with a
# "kex_exchange_identification" transcript that has nothing to do with what
# it was testing. Put such a scenario in one of the hostile halves
# (test-ssh-hostile-qemu.sh, test-ssh-hostile-hooks-qemu.sh), whose first
# server exempts the host address, or wait out the penalty first.
run_ssh_pw "$TEST_TMPDIR/pw-good.log"   pwuser   "$PW_OK"
run_ssh_pw "$TEST_TMPDIR/pw-bad.log"    pwuser   "$PW_WRONG"
# EMPTY PASSWORD FIRST, deliberately folded into an EXISTING connection
# rather than added as a new one: the paragraph above spells out that this
# file sits exactly on the shipped penalty threshold, and a fifth failing
# connection would be refused at accept. The change-required scenario is the
# right host -- its assertions are about the banner and about never
# authenticating, neither of which counts prompts -- and an empty password
# costs it nothing: it is refused before any hook and does NOT consume one of
# the three password-cap slots, so the three CHANGE_FIRST verdicts that end
# the connection still happen, at prompts 2-4 of ssh's 5.
ASKPASS_EMPTY_FIRST=1 \
run_ssh_pw "$TEST_TMPDIR/pw-change.log" changeme "$PW_CHANGE"
SSH_USER=nokey run_ssh "$TEST_TMPDIR/pk-nokey.log" -i "$TEST_TMPDIR/good"

pw_good()   { grep -q "$1" "$TEST_TMPDIR/pw-good.log"; }
pw_bad()    { grep -q "$1" "$TEST_TMPDIR/pw-bad.log"; }
pw_change() { grep -q "$1" "$TEST_TMPDIR/pw-change.log"; }
pk_nokey()  { grep -q "$1" "$TEST_TMPDIR/pk-nokey.log"; }

# Mirrors SUCCESS_RE above -- verified against a captured OpenSSH 9.9p1
# transcript in this run: `Authenticated to 127.0.0.1 ([127.0.0.1]:18521)
# using "password".`
#
# SECOND COPY in lib/ssh-hostile-lib.sh (both hostile halves). Change both.
SUCCESS_PW_RE='Authenticated to .* using "password"'

# Negatives as named functions, the pattern this file already uses for
# bad_never_authenticated -- not `bash -c` with nested quoting, where a
# pattern that silently fails to reach grep turns the check into a
# tautology.
pw_bad_never_authenticated()    { ! grep -q "$SUCCESS_PW_RE" "$TEST_TMPDIR/pw-bad.log"; }
pw_change_never_authenticated() { ! grep -q "$SUCCESS_PW_RE" "$TEST_TMPDIR/pw-change.log"; }

# "The server ended the connection" is a disjunction, not one string. Before
# P5 a queued SSH_MSG_DISCONNECT raced the abortive close behind it; the cap
# below now pins its exact line (CAP_DISC_RE, lib/ssh-guest-lib.sh), and
# this stays only for the
# NEGATIVE check that a refused key does not end the connection at all.
# SECOND COPY of this exact regex is lib/ssh-hostile-lib.sh's
# ENDED_RE (both hostile halves) -- change both.
#
# Verified against captured transcripts in this run:
#   pw-bad.log:    "Received disconnect from 127.0.0.1 port <p>:14: too many
#                    authentication failures" -- matches.
#   pk-nokey.log:  ssh exhausts publickey (the only method it offers here),
#                    prints "No more authentication methods to try." then
#                    "Permission denied (publickey,password)." with NO
#                    disconnect/reset/closed line anywhere in the transcript
#                    -- confirmed no match.
ENDED_RE='Received disconnect from|Connection reset|Connection closed by|Broken pipe|connection unexpectedly closed'

pk_nokey_not_disconnected() { ! grep -qE "$ENDED_RE" "$TEST_TMPDIR/pk-nokey.log"; }

# The verdict for pwuser is delivered two timer ticks after the hook
# returned, so this proves the ASYNCHRONOUS completion path end to end.
check "password: authenticates with an asynchronously delivered verdict" \
    pw_good "$SUCCESS_PW_RE"
check "password: exactly one prompt for a right password" \
    test "$(prompts "$TEST_TMPDIR/pw-good.log")" = 1

check "wrong password: never authenticated" pw_bad_never_authenticated
# THE CAP. ssh would prompt 5 times; 3 means the server ended it.
check "wrong password: the server ends the connection after 3 attempts" \
    test "$(prompts "$TEST_TMPDIR/pw-bad.log")" = 3
# Delivered since P5 (the linger), so the line is pinned whole -- the
# password cap sends sshd's own text (auth.c:343) with RFC 4253's code 14.
check "wrong password: ssh(1) printed the server's DISCONNECT, exactly" \
    grep -qaE "$CAP_DISC_RE" "$TEST_TMPDIR/pw-bad.log"

check "change-required: the banner reaches the client" \
    pw_change "password change required: use the web UI"
check "change-required: never authenticated" pw_change_never_authenticated

# A refused publickey PRINCIPAL is an ordinary failure, not forgery. The
# negative greps for the same ENDED_RE the wrong-password scenario above
# proves is really printed when a server disconnects.
check "publickey principal refused: denied" \
    pk_nokey "Permission denied"
check "publickey principal refused: failure, not disconnect" \
    pk_nokey_not_disconnected

# THE EMPTY PASSWORD (OpenSSH's PermitEmptyPasswords default is `no`). These
# two assertions read the GUEST log, not a transcript: refusing before the
# hook and refusing inside it look identical to the client, so only the
# server can say which happened. The fixture prints "SERVER: PW-LEN <n>" on
# every password-hook entry -- the length only, never a byte of the password.
#
# The positive comes FIRST and is what anchors the negative: it proves this
# fixture really does print the line, so "PW-LEN 0 never appears" cannot pass
# because the marker was misspelled or the fixture was never entered. 16 is
# rand_hex's output length (8 bytes as hex), shared by every generated
# password here.
test_clean_log
pwlen_real_seen()   { grep -qaxF 'SERVER: PW-LEN 16' "$TEST_CLEAN_LOG"; }
pwlen_zero_absent() { ! grep -qaxF 'SERVER: PW-LEN 0' "$TEST_CLEAN_LOG"; }

check "empty password PRECONDITION: the password hook really reports lengths" \
    pwlen_real_seen
check "empty password: never reaches the application hook" \
    pwlen_zero_absent

# THE INBOUND PAYLOAD BUFFER IS PER CONNECTION, NOT PER PACKET.
#
# Every encrypted packet used to be decrypted into a fresh
# axl_malloc(AXL_SSH_PACKET_MAX) -- 35,000 bytes, the RFC 4253 section 6.1
# ceiling, allocated before the plaintext length was known and freed after
# dispatch. That is the right LIMIT and the wrong allocation unit: it makes
# one keystroke cost a 35 KB round trip through the firmware allocator.
#
# What discriminates is SCALING, not a total: the fixture prints the
# allocator's cumulative totals on entry to every password hook, so the
# difference between two consecutive lines for ONE user is everything the
# server allocated to carry one more packet on the SAME connection. Three lines
# from three CONNECTIONS would not pass this by accident either -- each would
# carry its own fresh buffer and the deltas would be worse, not better. The
# changeme scenario is the one that gives three of them: its first prompt is
# empty (refused before any hook), and prompts 2-4 each reach the hook with a
# CHANGE_FIRST verdict until the cap ends the connection -- one connection,
# three hooks, nothing else running in between, because run_ssh_pw blocks.
#
# The bound is 35000 because that IS the quantity removed, not because it
# read well: a per-packet payload buffer cannot fit under it, and the measured
# difference is about twenty times below it. Printed with each delta so a
# regression shows its size rather than just failing.
#
# A NOTE FOR THE NEXT PERSON: "the payload pointer is the same address every
# packet" is NOT a substitute for this. axl_free of a 35,000-byte block
# followed by axl_malloc(35000) hands back the SAME address, so that
# assertion passes on the per-packet code it is supposed to catch.
PAYLOAD_MAX=35000     # AXL_SSH_PACKET_MAX, src/net/axl-ssh-internal.h
# The BOUND the assertion actually uses, and it is deliberately NOT
# PAYLOAD_MAX. Measured cost of one further packet on a live connection is
# 1,647 bytes, and the run-to-run variance the implementer recorded is about
# +/-144. A bound of PAYLOAD_MAX passes anything under 35,000, so a partial
# regression -- a future 16 KB per-packet allocation, or ONE of the two this
# change removed coming back in a smaller form -- would go green. 10,000 is
# 6x the observed value, far outside that variance, and still fails loudly
# on any reintroduction of a packet-sized buffer. The 35,000 stays above
# only to name what the old code cost.
PAYLOAD_REGRESSION_MAX=10000

# "SERVER: PW-ALLOC <count> <bytes> <user>", anchored at both ends and on the
# shape of both numbers: the name is the peer's to choose, so a loose match
# here would let a client's own name stand in for a fixture line.
mapfile -t PW_ALLOC < <(grep -aE '^SERVER: PW-ALLOC [0-9]+ [0-9]+ changeme$' \
    "$TEST_CLEAN_LOG" || true)

# ONE definition each, used by the assertion AND by the control below. That is
# the point: a control that named the column independently could agree with a
# broken extractor. Break either of these and the control fails.
alloc_count() { awk '{print $3}' <<< "$1"; }
alloc_bytes() { awk '{print $4}' <<< "$1"; }

# CONTROL 1 — the lines exist, and there are three of them. Without it the
# delta check below would pass vacuously on an empty list.
check "payload buffer PRECONDITION: three password hooks on one connection (got ${#PW_ALLOC[@]})" \
    test "${#PW_ALLOC[@]}" = 3

# CONTROL 2 — alloc_bytes really reads the BYTES column. Reading the count
# column instead is a one-character error that would make every delta ~20 and
# the assertion vacuously green, which is this tree's "a gate that cannot SEE"
# failure exactly. Anchored on nothing hand-picked: every counted allocation is
# at least one byte and the mean is tens, so the byte total always exceeds the
# allocation count -- and swapping the two extractors inverts it.
payload_alloc_columns_are_right() {
    local line
    for line in "${PW_ALLOC[@]}"; do
        if [[ "$(alloc_bytes "$line")" -le "$(alloc_count "$line")" ]]; then
            return 1
        fi
    done
    [[ "${#PW_ALLOC[@]}" -gt 0 ]]
}
check "payload buffer CONTROL: the graded column is bytes, not the allocation count" \
    payload_alloc_columns_are_right

payload_buffer_is_per_connection() {
    local i prev cur d ok=0
    if [[ "${#PW_ALLOC[@]}" -lt 2 ]]; then
        return 1
    fi
    for ((i = 1; i < ${#PW_ALLOC[@]}; i++)); do
        prev=$(alloc_bytes "${PW_ALLOC[i-1]}")
        cur=$(alloc_bytes "${PW_ALLOC[i]}")
        d=$((cur - prev))
        echo "      packet $i on that connection cost ${d} bytes"
        if [[ "$d" -ge "$PAYLOAD_REGRESSION_MAX" ]]; then
            ok=1
        fi
    done
    return "$ok"
}
check "payload buffer: a further packet on one connection costs no 35 KB allocation" \
    payload_buffer_is_per_connection

# THE AUTHENTICATION AUDIT LOG (sshd's `Accepted`/`Failed ... from A port P`
# at its default LogLevel INFO). Also guest-side: it is the server's record,
# and nothing a client sees can stand in for it. 10.2.2 is QEMU user-mode
# networking's gateway -- the host as the guest sees it -- so it is the
# source address every connection in this file carries.
#
# Both spellings are checked, because the two halves were written apart and
# a method name that reads "public key" in one and "publickey" in the other
# is exactly the drift an audit consumer trips over.
audit_pk_success() {
    grep -qa 'ssh: Accepted publickey for root from 10\.0\.2\.2 port [0-9]' \
        "$TEST_CLEAN_LOG"
}
audit_pw_success() {
    grep -qa 'ssh: Accepted password for pwuser from 10\.0\.2\.2 port [0-9]' \
        "$TEST_CLEAN_LOG"
}
audit_failure() {
    grep -qa 'ssh: Failed .* for .* from 10\.0\.2\.2 port [0-9]' \
        "$TEST_CLEAN_LOG"
}
check "audit log: a publickey login is recorded with user and source" \
    audit_pk_success
check "audit log: a password login is recorded with user and source" \
    audit_pw_success
check "audit log: a FAILURE is recorded (it was logged nowhere before)" \
    audit_failure

# THE PASSWORD CAP ENDING A CONNECTION IS ITS OWN RECORD, as sshd's
# MaxAuthTries is: "maximum authentication attempts exceeded for %s%.100s
# from %.200s port %d ssh2" (/usr/libexec/openssh/sshd-session, OpenSSH
# 9.9p1). pw-bad's third wrong password is the first cap this server hits,
# so its line is at INFO with no suppressed count -- pinned whole, timestamp
# to port, so neither a DEBUG copy nor a line carrying the rate limiter's
# "(and N more ...)" suffix satisfies it. Until 2026-09-20 the "Failed
# password" lines were rate-limited away and nothing said the connection had
# been thrown out at all.
audit_pw_exceeded() {
    grep -qaxE '[0-9:.]+ \[INFO\]  ssh: maximum authentication attempts exceeded for pwuser from 10\.0\.2\.2 port [0-9]+' \
        "$TEST_CLEAN_LOG"
}
check "audit log: the third wrong password's disconnect is recorded, with user and source" \
    audit_pw_exceeded

# THE RATE LIMIT, measured rather than asserted from the code. This file
# refuses eleven real attempts across six connections -- ssh-peer.py's one
# wrong password at the top (added in P4a; the ceiling below did not
# need to move, since one more refusal inside the window prints no more
# INFO lines), one unauthorized key,
# one more in the bad-then-good list, three wrong passwords, one empty
# password, three change-required verdicts, and one refused publickey
# principal. Unlogged before; unbounded, that is eleven INFO lines a peer can
# put on a firmware console per connection round.
#
# The ceiling is 3, not 1, deliberately. The window is
# AXL_SSH_AUTH_LOG_WINDOW_MS (60 s) and these ssh runs are back to back, so
# the honest expectation is 1 -- but under a loaded parallel pool the span
# from the first refusal to the last can cross a window boundary, and a test
# that fails for being slow teaches nothing. 3 still discriminates: without
# the limiter this is 11.
n_audit_fail=$(grep -ac 'ssh: Failed .* for .* from 10\.0\.2\.2 port [0-9]' \
    "$TEST_CLEAN_LOG" || true)
check "audit log: eleven refused attempts, at most 3 INFO lines (got ${n_audit_fail})" \
    test "${n_audit_fail:-0}" -le 3

# THE TRANSMIT QUEUE: one TCP send in flight per connection, and everything
# sealed while it is out -- or in the same loop turn -- leaves in the next one.
# Graded on the server's own per-send DEBUG line, whose tail is pinned whole:
# "tx: F frames, B bytes". The prefix carries the function and source line
# ("ssh:tx_flush:880:"), which moves with every edit, so it is matched by
# shape and only the text after it is compared exactly.
TX_RE='^[0-9:.]+ \[DEBUG\] ssh:[A-Za-z_]+:[0-9]+: tx: [0-9]+ frames, [0-9]+ bytes$'
# tx_tail: the `tx:` lines of its input, reduced to their pinned tail.
# tx_lines: the same for clean-log lines (FROM, TO]. Line numbers survive
# test_clean_log (it edits within lines and deletes only '\r'), so a mark
# taken on $TEST_LOG addresses the same line here.
tx_tail() {
    { grep -aE "$TX_RE" || true; } \
        | sed -E 's/^.*: (tx: [0-9]+ frames, [0-9]+ bytes)$/\1/'
}
tx_lines() { sed -n "$(($1 + 1)),$2p" "$TEST_CLEAN_LOG" | tx_tail; }
tx_frames() { sed -E 's/^tx: ([0-9]+) frames.*/\1/'; }

# (1) ssh(1)'s own connection, the oracle. Its FIRST send is the
# identification line and KEXINIT together -- they are queued at accept, and
# ssh(1) sends nothing that draws a reply until it has our KEXINIT -- and its
# SECOND is KEX_ECDH_REPLY with NEWKEYS, which one parsed packet produces. The
# sizes are measured and fixed: a 20-byte identification line plus a
# 200-byte KEXINIT frame, and the Ed25519 reply plus NEWKEYS. Before the queue
# existed there were no `tx:` lines at all, and a sender that sent each frame
# on its own would print "tx: 1 frames" here.
#
# THESE ARE ALSO THE CONTROL for the burst count below: they prove the
# extraction really sees a multi-frame send, so "largest >= 10" cannot pass
# or fail because the counter misreads the line.
mapfile -t TX_GOOD < <(tx_lines "$TX_MARK_GOOD0" "$TX_MARK_GOOD1")
check "tx: ssh(1)'s first send is identification + KEXINIT (got '${TX_GOOD[0]:-none}')" \
    test "${TX_GOOD[0]:-}" = "tx: 2 frames, 220 bytes"
check "tx: ssh(1)'s second send is KEX_ECDH_REPLY + NEWKEYS (got '${TX_GOOD[1]:-none}')" \
    test "${TX_GOOD[1]:-}" = "tx: 2 frames, 208 bytes"

# (2) The burst: fifty CHANNEL_OPENs arrive in one sendall and are parsed in
# as few turns as TCP delivers them in, so their fifty 44-byte replies must
# leave in a few sends, not fifty. The slice's tx lines after the
# "Accepted password" record are USERAUTH_SUCCESS (one frame, alone: the peer
# waits for it before bursting) and then the burst's replies. Those must sum
# to EXACTLY 50 -- none lost, none duplicated, none borrowed from another
# connection -- and the largest send must carry at least 10: about fifteen of
# these frames fit one MSS. Per-packet sending gives fifty lines of 1.
mapfile -t TX_BURST < <(sed -n "$((TX_MARK_BURST0 + 1)),${TX_MARK_BURST1}p" \
    "$TEST_CLEAN_LOG" \
    | sed -n '/ssh: Accepted password for pwuser from /,$p' | tx_tail)
check "tx: USERAUTH_SUCCESS leaves on its own before the burst (got '${TX_BURST[0]:-none}')" \
    test "${TX_BURST[0]:-}" = "tx: 1 frames, 28 bytes"
TX_BURST_SUM=0; TX_BURST_MAX=0; TX_BURST_SENDS=0
for l in "${TX_BURST[@]:1}"; do
    TX_BURST_SENDS=$((TX_BURST_SENDS + 1))
    f=$(tx_frames <<<"$l")
    TX_BURST_SUM=$((TX_BURST_SUM + f))
    if [[ "$f" -gt "$TX_BURST_MAX" ]]; then TX_BURST_MAX=$f; fi
done
check "tx: the burst's replies total exactly 50 frames (got $TX_BURST_SUM in $TX_BURST_SENDS sends)" \
    test "$TX_BURST_SUM" = 50
check "tx: the burst's largest send carries at least 10 frames (got $TX_BURST_MAX)" \
    test "$TX_BURST_MAX" -ge 10

# (3) Never at INFO: one line per send would put a line per keystroke on a
# firmware console. The positive twin is (1), which proves the line exists.
tx_never_at_info() {
    ! grep -qaE '\[INFO\] .*tx: [0-9]+ frames, [0-9]+ bytes' "$TEST_CLEAN_LOG"
}
check "tx: the per-send line is never written at INFO" tx_never_at_info

# Graded on the log as it stands after the last handshake: this suite does
# not wait for SERVER: DONE (its fixture deadline is a fixed guard, and
# waiting it out would roughly double the run), so a drop at the deadline
# or in axl_ssh_server_free is NOT covered here. The hostile-lib suites and
# policy wait for DONE and cover it.
ssh_check_no_reentry "through the last handshake (shutdown not graded)"

if [[ "$fail" -ne 0 ]]; then
    for f in "$GOOD_LOG" "$BAD_LOG" "$SEQ_LOG" "$UNIMPL_LOG" "$KEEPALIVE_LOG" \
             "$TEST_TMPDIR/pw-good.log" "$TEST_TMPDIR/pw-bad.log" \
             "$TEST_TMPDIR/pw-change.log" "$TEST_TMPDIR/pk-nokey.log"; do
        echo "--- $(basename "$f") (tail) ---"
        tail -30 "$f" | sed 's/^/      /'
    done
    printf -- '--- ssh-peer.py ---\n      good: %s\n      burst (rc=%s): %s\n      expectn control (rc=%s): %s\n      bad (rc=%s): %s\n      kexwait (rc=%s): %s\n      unimpl: %s\n' \
        "$PEER_OK" "$PEER_BURST_RC" "${PEER_BURST//$'\n'/ | }" \
        "$PEER_NOTN_RC" "${PEER_NOTN//$'\n'/ | }" \
        "$PEER_BAD_RC" "$PEER_BAD" "$PEER_KEX_RC" "${PEER_KEX//$'\n'/ | }" \
        "${UNIMPL_OUT//$'\n'/ | }"
    test_clean_log
    echo "--- guest serial log (tail) ---"
    tail -20 "$TEST_CLEAN_LOG" | sed 's/^/      /'
fi

echo
echo "ssh auth: $pass passed, $fail failed ($TEST_ARCH)"
[[ "$fail" -eq 0 ]]
