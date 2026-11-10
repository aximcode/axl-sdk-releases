#!/bin/bash
# test-meta: arch=both needs=ssh est=50 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-rekey-hostile-qemu.sh -- AxlSsh key re-exchange driven at the
# byte level by ssh-peer.py's rekey engine (its module doc, REKEY): the
# orders ssh(1) never produces -- data behind the peer's KEXINIT, a second
# KEXINIT, a wrong guess, a second KEX_ECDH_INIT, a packet sealed under the
# old key right before NEWKEYS, crossed KEXINITs, a rekey with a zero window
# -- and the one thing ssh(1) cannot SEE: the server's held output staying
# off the wire until its NEWKEYS. test-ssh-rekey-qemu.sh drives the same
# server with a real ssh(1).
#
# EVERY EXPECTATION FOLLOWS sshd (OpenSSH 9.9p1), not the RFC's strictest
# reading: after the peer's KEXINIT channel messages are still PROCESSED
# (kex_reset_dispatch resets 1-49 only); a second KEXINIT, a second
# KEX_ECDH_INIT and a transport message outside the exchange's set draw
# UNIMPLEMENTED and the exchange carries on (kex_protocol_error, kex.c and
# kexgen.c); the server's own messages 50+ (and 5, 6, 7) are held from its
# KEXINIT to its NEWKEYS (packet.c ssh_packet_send2). src/net/axl-ssh-rekey.h
# cites each.
#
# THE WIRE CHECK. Between the server's KEXINIT and its NEWKEYS the peer
# accepts only what sshd may send there (ssh-peer.py's sendable_mid_kex);
# anything else ends it as `peer: got T inside the server's exchange`, exit
# 3. Every scenario below runs under it; held-on-the-wire exists to drive
# it with real held output. ssh(1) processes channel messages mid-exchange,
# so a server that flushed its held packets under the OLD key before its
# NEWKEYS looked healthy to the ssh(1) suite.
#
# The guest runs ssh-server-selftest.efi with `channels=1` (the fixture
# shell), `sshloglevel=debug` (every rekey line is DEBUG, as sshd logs
# rekeying at debug only) and rekeylimit=65536. Setup and the teardown
# checks are lib/ssh-hostile-lib.sh's; the peer helpers and message bodies
# lib/ssh-channel-lib.sh's.
#
# Scenarios, in run order:
#   rekey-control     THE CONTROL for the peer's engine: `--tamper hash`
#                     (the first exchange's KEXINITs hashed) must fail the
#                     signature check, `--tamper sid` (this exchange's H as
#                     the session id) must kill the connection -- so the
#                     engine's own checks are seen to fail before any of its
#                     passes is trusted
#   peer-rekey-then-data
#                     two peer-started exchanges, an echo answered after
#                     each
#   data-after-peer-kexinit
#                     KEXINIT, then CHANNEL_DATA and SERVICE_REQUEST before
#                     KEX_ECDH_INIT: the data reaches the shell (its echo
#                     arrives after the server's NEWKEYS), SERVICE_REQUEST
#                     draws UNIMPLEMENTED, the exchange completes
#   server-rekey-crossing
#                     the server's inbound count crosses the limit inside
#                     a 96 KB pour that the peer's KEXINIT follows in the
#                     stream: both KEXINITs cross on the wire, ONE exchange
#   rekey-under-backpressure
#                     a zero-window channel holding queued output, a
#                     server-started rekey (IGNOREs carry the inbound
#                     count over), then the adjust drains it all
#   in-flight-across-switch
#                     CHANNEL_DATA sealed under the old key and NEWKEYS in
#                     ONE write: delivered
#   second-kexinit-while-active
#                     a second KEXINIT mid-exchange: UNIMPLEMENTED naming
#                     it, the exchange completes
#   held-on-the-wire  300 KB of shell output across several server-started
#                     exchanges, each holding output, under the wire check
#   wrong-guess-rekey a rekey KEXINIT with a wrong guess and the guessed
#                     packet: that packet is dropped, the exchange completes
#   kex-replied-guard a second KEX_ECDH_INIT after the REPLY: UNIMPLEMENTED,
#                     the exchange completes
#   held-cap          the peer's KEXINIT, then 8000 CHANNEL_OPENs and never
#                     its KEX_ECDH_INIT: the held replies pass 128 KiB and
#                     the connection is dropped -- logged, no leak, no crash
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-rekey-hostile-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh rekey hostile"
HOSTILE_SCENARIOS="rekey-control peer-rekey-then-data data-after-peer-kexinit server-rekey-crossing rekey-under-backpressure in-flight-across-switch second-kexinit-while-active held-on-the-wire wrong-guess-rekey kex-replied-guard held-cap"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh). Measured 2026-10-09, standalone: every
# scenario 0-3 s on aa64 (TCG), 0-2 s on x64 -- the peer's pure-Python
# crypto and the logins, not the guest. The longest on aa64 are held-cap
# and rekey-under-backpressure (3 s). Each budget is at least twice its
# measurement and at least 3 s, for a loaded pool.
declare -A HOSTILE_BUDGET=(
    [rekey-control]=4 [peer-rekey-then-data]=3 [data-after-peer-kexinit]=3
    [server-rekey-crossing]=4 [rekey-under-backpressure]=6
    [in-flight-across-switch]=3 [second-kexinit-while-active]=3
    [held-on-the-wire]=6 [wrong-guess-rekey]=3 [kex-replied-guard]=3
    [held-cap]=8
)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
REKEY_LIMIT=65536
HOSTILE_EXTRA_ARGS="channels=1 sshloglevel=debug rekeylimit=$REKEY_LIMIT"

hostile_boot "AxlSsh rekey against hostile peers"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"

# --- helpers ------------------------------------------------------------------

took() { echo "  $1 took $((SECONDS - t0))s"; }

# The server's rekey lines, whole: DEBUG, so the console timestamp, the
# level and `ssh:func:line: ` come first (measured).
DBG='[0-9:.]+ \[DEBUG\] ssh:[a-z_0-9]+:[0-9]+: '
DONE_RE="${DBG}rekey: complete"
PEER_RE="${DBG}rekey: started by the peer"
OURS_RE="${DBG}rekey: started by us, [0-9]+ bytes out, [0-9]+ in"
HELD_RE="${DBG}rekey: sealed [1-9][0-9]* held after our NEWKEYS"
refused_re() { echo "${DBG}rekey: message $1 refused mid-exchange"; }

# The shell's start, and its lines in a transcript after `authenticated`.
SHELL_START="send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93"
shell_lines() {
    printf '%s\n' "$OPENED" "$(got91 "$1")" 'peer: sent 98 seq=3' \
        'peer: got 99' 'peer: got 93 bytes=2097152'
}
# The line shell's answer to `echo T`: the input echoed, CRLF, T, CRLF.
echo_line() { printf '%s\n' "peer: data out \"echo $1\\r\\n$1\\r\\n\""; }
ECHO_LEN=11   # for a one-character T
# Transcript of peer $1 is exactly: authenticated, the shell's start, then
# the lines given; exit code $2.
shell_transcript() {
    local name="$1" rc="$2"
    shift 2
    [[ "$PEER_RC" == "$rc" ]] \
        && [[ "$(cat "$TEST_TMPDIR/ssh-$name.log")" \
              == "$(printf '%s\n' 'peer: authenticated'; shell_lines "$name"; printf '%s\n' "$@")" ]]
}
# The same transcript with every `peer: rekeyed, started by the server`
# line taken out (their number is checked on its own), and that number.
without_server_rekeys() { grep -vxF 'peer: rekeyed, started by the server' "$TEST_TMPDIR/ssh-$1.log" || true; }
server_rekeys() { local n; n=$(grep -cxF 'peer: rekeyed, started by the server' "$TEST_TMPDIR/ssh-$1.log" || true); echo "${n:-0}"; }

# Wait for the connection's principal release: the last line a connection
# prints, so its rekey lines are all in the log by then.
settled() {
    check "$2: the connection ended and released its principal" \
        wait_since "$1" 1 10 'SERVER: PRINCIPAL-RELEASED'
}

# --- rekey-control -------------------------------------------------------------
#
# The engine's checks must be seen to FAIL before their passes mean anything.
# `--tamper hash` hashes the FIRST exchange's two KEXINITs -- what a peer
# that never replaced them would hash -- and the server's signature, made
# over THIS exchange's, must not verify: `peer: bad host signature`, exit 6.
# `--tamper sid` derives the new keys with this exchange's H where the
# ORIGINAL session id belongs: both sides then send under keys the other
# does not hold, and the server ends the connection on the peer's first
# packet under them.
if wants rekey-control; then
    t0=$SECONDS
    m=$(mark)
    peer tamper-hash "$SHELL_START,rekey" --tamper hash
    check "rekey-control: a rekey hashed over the first exchange's KEXINITs fails the host signature (exit 6)" \
        shell_transcript tamper-hash 6 'peer: bad host signature'
    peer tamper-sid "$SHELL_START,rekey,send:94:$(data $'echo s\r'),expectdata:$ECHO_LEN" --tamper sid
    echo "  rekey-control: --tamper sid ended with rc $PEER_RC: $(tail -1 "$TEST_TMPDIR/ssh-tamper-sid.log")"
    check "rekey-control: keys derived with the wrong session id end the connection (exit 3 or 4, never 0)" \
        test "$PEER_RC" = 3 -o "$PEER_RC" = 4
    check "rekey-control: both connections ended and released their principals" \
        wait_since "$m" 2 10 'SERVER: PRINCIPAL-RELEASED'
    check "rekey-control: ...the server's side: the exchange completed, then the peer's first packet under its keys ended it" \
        test "$(since "$m" | grep -xE -- "${DBG}(rekey: complete|disconnect: .*)" \
                  | sed -E 's/^.*: (rekey|disconnect): /\1: /' || true)" \
            = "$(printf '%s\n' 'rekey: complete' 'disconnect: protocol error')"
    took rekey-control
fi

# --- peer-rekey-then-data ---------------------------------------------------------
if wants peer-rekey-then-data; then
    t0=$SECONDS
    m=$(mark)
    peer peer-rekey "$SHELL_START,rekey,send:94:$(data $'echo a\r'),expectdata:$ECHO_LEN,rekey,send:94:$(data $'echo b\r'),expectdata:$ECHO_LEN,send:97:$RCPT,expect:97"
    settled "$m" peer-rekey-then-data
    check "peer-rekey-then-data: two exchanges, an echo answered after each" \
        shell_transcript peer-rekey 0 'peer: rekeyed, started by us' \
            'peer: sent 94 seq=0' "$(echo_line a)" \
            'peer: rekeyed, started by us' \
            'peer: sent 94 seq=0' "$(echo_line b)" \
            'peer: sent 97 seq=1' 'peer: got 97'
    check "peer-rekey-then-data: the server saw both started by the peer and completed both" \
        test "$(since_re "$m" "$PEER_RE")" = 2 -a "$(since_re "$m" "$DONE_RE")" = 2 \
            -a "$(since_re "$m" "$OURS_RE")" = 0
    took peer-rekey-then-data
fi

# --- data-after-peer-kexinit ------------------------------------------------------
#
# The peer's KEXINIT, then CHANNEL_DATA and SERVICE_REQUEST, then the rest of
# the exchange. sshd processes the data -- its kex resets only the 1-49
# dispatch entries -- and the shell's echo of it is HELD behind the
# server's KEXINIT, so it arrives after the server's NEWKEYS (the wire check
# enforces that). SERVICE_REQUEST is a transport message outside the
# exchange's set: UNIMPLEMENTED naming its seq (sshd: dispatch_protocol_error
# on a first rekey), sent at once -- it is a transport message, so it is not
# held -- and the connection goes on.
if wants data-after-peer-kexinit; then
    t0=$SECONDS
    m=$(mark)
    SVC="0000000c$(printf '%s' ssh-userauth | od -An -tx1 | tr -d ' \n')"
    peer data-after "$SHELL_START,kexinit,send:94:$(data $'echo y\r'),send:5:$SVC,rekey,expect:3,expectdata:$ECHO_LEN,send:94:$(data $'echo z\r'),expectdata:$ECHO_LEN,send:97:$RCPT,expect:97"
    settled "$m" data-after-peer-kexinit
    check "data-after-peer-kexinit: the data was processed, SERVICE_REQUEST drew UNIMPLEMENTED (its seq), the exchange completed, the session goes on" \
        shell_transcript data-after 0 'peer: sent 20 seq=4' \
            'peer: sent 94 seq=5' 'peer: sent 5 seq=6' \
            'peer: rekeyed, started by us' 'peer: got 3 seq=6' \
            "$(echo_line y)" 'peer: sent 94 seq=0' "$(echo_line z)" \
            'peer: sent 97 seq=1' 'peer: got 97'
    check "data-after-peer-kexinit: the guest refused message 5 mid-exchange, once, and completed one exchange the peer started" \
        test "$(since_re "$m" "$(refused_re 5)")" = 1 -a "$(since_re "$m" "$PEER_RE")" = 1 \
            -a "$(since_re "$m" "$DONE_RE")" = 1
    took data-after-peer-kexinit
fi

# --- server-rekey-crossing ---------------------------------------------------------
#
# Review Focus 3, both KEXINITs in flight at once. The peer pours 96 KB into
# `sink` and writes its KEXINIT right behind it; the server's INBOUND count
# passes 65536 inside the pour, so it sends its own KEXINIT before it reads
# the peer's -- and the peer has sent its own before reading the server's.
# Each side then takes the other's as the answer (sshd's kex_input_kexinit
# sends its own only if not already sent): ONE exchange, started by the
# server's count, nothing started by the peer's KEXINIT.
if wants server-rekey-crossing; then
    t0=$SECONDS
    m=$(mark)
    cr_sink=$'\n'"sink 98304 $(nsum x 98304)"$'\n'
    peer crossing "send:90:$OPEN,expect:91,send:98:$(exec_req sink),expect:99,expect:93,pour:98304:16384,kexinit,rekey,send:96:$RCPT,expectdata:${#cr_sink},expect:98,expect:96,expect:97,send:97:$RCPT"
    settled "$m" server-rekey-crossing
    check "server-rekey-crossing: the pour, the crossed KEXINITs, one exchange, then the sink's count of every byte" \
        transcript crossing 0 "$OPENED" "$(got91 crossing)" 'peer: sent 98 seq=3' \
            'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 98304 in 6 packets seqs=4..9' 'peer: sent 20 seq=10' \
            'peer: rekeyed, started by us' 'peer: sent 96 seq=0' \
            "peer: data out \"\\nsink 98304 $(nsum x 98304)\\n\"" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=1'
    cr_in=$( { since "$m" | grep -oE 'started by us, [0-9]+ bytes out, [0-9]+ in$' \
        || true; } | awk -v l="$REKEY_LIMIT" '$7 > l {n++} END{print n+0}')
    check "server-rekey-crossing: exactly one exchange, the server's (on its inbound count, $cr_in), none started by the peer's KEXINIT" \
        test "$(since_re "$m" "$OURS_RE")" = 1 -a "$cr_in" = 1 \
            -a "$(since_re "$m" "$PEER_RE")" = 0 -a "$(since_re "$m" "$DONE_RE")" = 1
    took server-rekey-crossing
fi

# --- rekey-under-backpressure -----------------------------------------------------
#
# Review Focus 4. A channel opened with window 0: the shell's `flood 300000`
# queues up to the per-channel limit and nothing can leave. Five 16,000-byte
# IGNOREs carry the server's INBOUND count past the limit, so the server
# starts an exchange with that output stuck behind a closed window; the
# exchange must complete anyway (`rekey:server` waits for and answers it),
# and only then does the peer's adjust let all of it out, exact.
if wants rekey-under-backpressure; then
    t0=$SECONDS
    m=$(mark)
    bp_ign=$(head -c 16000 /dev/zero | od -An -tx1 -v | tr -d ' \n')
    peer backpressure "send:90:$OPEN_Z,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'flood 300000\r'),hold:1,burst:5:2:$bp_ign,rekey:server,send:93:$(hexof chan-adjust-hex --recipient @ --bytes 1048576),expectsum:300014,send:97:$RCPT,expect:97"
    settled "$m" rekey-under-backpressure
    bp_more=$(server_rekeys backpressure)
    echo "  backpressure: $bp_more exchange(s) the server started, the peer's lines"
    check "rekey-under-backpressure: the server's exchange completed with the window shut, then the adjust drained all 300014 bytes" \
        test "$PEER_RC" = 0 -a "$(without_server_rekeys backpressure)" \
            = "$(printf '%s\n' 'peer: authenticated' "$OPENED" "$(got91 backpressure)" \
                'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
                'peer: sent 94 seq=4' 'peer: sent burst 5 x 2 seqs=5..9' \
                'peer: sent 93 seq=0' \
                "peer: sum out=300014 err=0 sha256=$(nsum x 300000 $'flood 300000\r\n')" \
                'peer: sent 97 seq=0' 'peer: got 97')"
    bp_first=$(since "$m" | grep -nxE -- "$DONE_RE" | head -1 | cut -d: -f1 || true)
    bp_flood=$(since "$m" | grep -nxF 'SERVER: FLOOD-DONE 300000' | head -1 | cut -d: -f1 || true)
    check "rekey-under-backpressure: the first exchange was the server's, on its inbound count, and completed before the flood could finish (lines ${bp_first:-none} < ${bp_flood:-none})" \
        test -n "$bp_first" -a -n "$bp_flood" -a "${bp_first:-0}" -lt "${bp_flood:-0}" \
            -a "$(since_re "$m" "$PEER_RE")" = 0
    check "rekey-under-backpressure: every exchange the server started completed, and the peer answered each ($bp_more after the first)" \
        test "$(since_re "$m" "$OURS_RE")" = "$(since_re "$m" "$DONE_RE")" \
            -a "$(since_re "$m" "$DONE_RE")" = "$bp_more"
    took rekey-under-backpressure
fi

# --- in-flight-across-switch -------------------------------------------------------
#
# Review Focus 1 from the peer's side. After the server's REPLY the peer
# seals CHANNEL_DATA under its OLD key and NEWKEYS right behind it, in ONE
# write -- a legal order, a pipelining client's. The server must open the
# data with the old key (its staged one is for after the peer's NEWKEYS)
# and deliver it.
if wants in-flight-across-switch; then
    t0=$SECONDS
    m=$(mark)
    peer in-flight "$SHELL_START,kexinit,rekey:reply,batch:94:$(data $'echo w\r')+21:,rekey,expectdata:$ECHO_LEN,send:94:$(data $'echo v\r'),expectdata:$ECHO_LEN,send:97:$RCPT,expect:97"
    settled "$m" in-flight-across-switch
    check "in-flight-across-switch: data sealed under the old key just before NEWKEYS was delivered" \
        shell_transcript in-flight 0 'peer: sent 20 seq=4' \
            'peer: rekey reply verified' 'peer: sent batch 94+21 seqs=6..7' \
            'peer: rekeyed, started by us' "$(echo_line w)" \
            'peer: sent 94 seq=0' "$(echo_line v)" \
            'peer: sent 97 seq=1' 'peer: got 97'
    check "in-flight-across-switch: one exchange, the peer's, completed" \
        test "$(since_re "$m" "$PEER_RE")" = 1 -a "$(since_re "$m" "$DONE_RE")" = 1
    took in-flight-across-switch
fi

# --- second-kexinit-while-active ---------------------------------------------------
#
# A second KEXINIT inside one exchange. sshd: kex_input_kexinit sets the
# KEXINIT entry to kex_protocol_error (kex.c:623), which answers
# UNIMPLEMENTED and, outside the strict initial exchange, goes on. The first
# KEXINIT is the one hashed.
if wants second-kexinit-while-active; then
    t0=$SECONDS
    m=$(mark)
    peer second-kexinit "$SHELL_START,kexinit,kexinit,rekey,expect:3,send:94:$(data $'echo k\r'),expectdata:$ECHO_LEN,send:97:$RCPT,expect:97"
    settled "$m" second-kexinit-while-active
    check "second-kexinit-while-active: UNIMPLEMENTED names the second KEXINIT, and the exchange completes" \
        shell_transcript second-kexinit 0 'peer: sent 20 seq=4' \
            'peer: sent 20 seq=5' 'peer: rekeyed, started by us' \
            'peer: got 3 seq=5' 'peer: sent 94 seq=0' "$(echo_line k)" \
            'peer: sent 97 seq=1' 'peer: got 97'
    check "second-kexinit-while-active: the guest refused message 20 mid-exchange, once, and completed one exchange" \
        test "$(since_re "$m" "$(refused_re 20)")" = 1 -a "$(since_re "$m" "$DONE_RE")" = 1
    took second-kexinit-while-active
fi

# --- held-on-the-wire --------------------------------------------------------------
#
# Ruling R4. 300 KB of shell output against the 64 KB limit: the server
# starts exchange after exchange from its SEND path, so each holds the
# packet whose sending started it (and whatever else it produces) until its
# NEWKEYS. The peer answers each as ssh(1) would, mid-read, under the wire
# check -- one held packet sealed before the server's NEWKEYS is
# `peer: got 94 inside the server's exchange`. Holding is REQUIRED, not
# assumed: at least one exchange must have sealed held output.
if wants held-on-the-wire; then
    t0=$SECONDS
    m=$(mark)
    peer held-wire "$SHELL_START,send:94:$(data $'flood 300000\r'),expectsum:300014,send:97:$RCPT,expect:97"
    settled "$m" held-on-the-wire
    hw_peer=$(server_rekeys held-wire)
    hw_ours=$(since_re "$m" "$OURS_RE")
    hw_done=$(since_re "$m" "$DONE_RE")
    hw_held=$(since_re "$m" "$HELD_RE")
    echo "  held-on-the-wire: the server started $hw_ours, completed $hw_done, $hw_held holding output; the peer answered $hw_peer"
    check "held-on-the-wire: no message 50+ (or 5, 6, 7) crossed the wire inside any of the server's exchanges; output exact" \
        test "$PEER_RC" = 0 -a "$(without_server_rekeys held-wire)" \
            = "$(printf '%s\n' 'peer: authenticated' "$OPENED" "$(got91 held-wire)" \
                'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
                'peer: sent 94 seq=4' \
                "peer: sum out=300014 err=0 sha256=$(nsum x 300000 $'flood 300000\r\n')" \
                'peer: sent 97 seq=0' 'peer: got 97')"
    check "held-on-the-wire: at least 3 exchanges, all the server's, all completed, each answered by the peer ($hw_ours / $hw_done / $hw_peer)" \
        test "$hw_ours" -ge 3 -a "$hw_done" = "$hw_ours" -a "$hw_peer" = "$hw_ours" \
            -a "$(since_re "$m" "$PEER_RE")" = 0
    check "held-on-the-wire: an exchange held output and sealed it after its NEWKEYS ($hw_held)" \
        test "$hw_held" -ge 1
    took held-on-the-wire
fi

# --- wrong-guess-rekey -------------------------------------------------------------
#
# A rekey KEXINIT with first_kex_packet_follows set and a guess the server
# rejects (a method it lacks listed first), and the guessed packet behind
# it: a type-30 group14 KEXDH_INIT. RFC 4253 section 7 and sshd
# (dispatch_skip_packets, kex.c kex_choose_conf) drop exactly that one
# packet; taken as a KEX_ECDH_INIT it is malformed and would end the
# connection. The real KEX_ECDH_INIT follows and the exchange completes.
if wants wrong-guess-rekey; then
    t0=$SECONDS
    m=$(mark)
    peer wrong-guess "$SHELL_START,kexinit:guess,rekey,send:94:$(data $'echo g\r'),expectdata:$ECHO_LEN,send:97:$RCPT,expect:97"
    settled "$m" wrong-guess-rekey
    check "wrong-guess-rekey: the guessed packet was dropped and the exchange completed" \
        shell_transcript wrong-guess 0 'peer: sent 20 seq=4' 'peer: sent 30 seq=5' \
            'peer: rekeyed, started by us' 'peer: sent 94 seq=0' "$(echo_line g)" \
            'peer: sent 97 seq=1' 'peer: got 97'
    check "wrong-guess-rekey: one exchange, the peer's, completed; nothing refused" \
        test "$(since_re "$m" "$PEER_RE")" = 1 -a "$(since_re "$m" "$DONE_RE")" = 1 \
            -a "$(since_re "$m" "${DBG}rekey: message [0-9]+ refused mid-exchange")" = 0
    took wrong-guess-rekey
fi

# --- kex-replied-guard -------------------------------------------------------------
#
# A second KEX_ECDH_INIT after the server's REPLY. sshd re-arms the message
# as kex_protocol_error on RECEIPT of the first INIT (kexgen.c:288), so the
# second is UNIMPLEMENTED and the exchange goes on. Here that is the
# kex_replied guard; without it the second INIT would be a second do_kex on
# a connection with a key already staged.
if wants kex-replied-guard; then
    t0=$SECONDS
    m=$(mark)
    kr_init="00000020$(printf '%064d' 0 | tr 0 1)"
    peer kex-replied "$SHELL_START,kexinit,rekey:reply,send:30:$kr_init,rekey,expect:3,send:94:$(data $'echo r\r'),expectdata:$ECHO_LEN,send:97:$RCPT,expect:97"
    settled "$m" kex-replied-guard
    check "kex-replied-guard: the second KEX_ECDH_INIT drew UNIMPLEMENTED naming it, and the exchange completed" \
        shell_transcript kex-replied 0 'peer: sent 20 seq=4' \
            'peer: rekey reply verified' 'peer: sent 30 seq=6' \
            'peer: rekeyed, started by us' 'peer: got 3 seq=6' \
            'peer: sent 94 seq=0' "$(echo_line r)" \
            'peer: sent 97 seq=1' 'peer: got 97'
    check "kex-replied-guard: the guest refused message 30 mid-exchange, once, and completed one exchange" \
        test "$(since_re "$m" "$(refused_re 30)")" = 1 -a "$(since_re "$m" "$DONE_RE")" = 1
    took kex-replied-guard
fi

# --- held-cap ----------------------------------------------------------------------
#
# The 128 KiB bound on held output (REKEY_HELD_MAX, axl-ssh-server.c), which
# sshd does not have. The peer sends its KEXINIT -- the server answers and
# holds from there -- then 8000 CHANNEL_OPENs of a type nobody serves, and
# its KEX_ECDH_INIT only queues behind the 8000 opens, so the cap is
# reached first. Each open draws a CHANNEL_OPEN_FAILURE that is
# held: 21 bytes a reply with its length prefix, so the 6,242nd passes the
# bound and the server drops the connection. Logged, at DEBUG; the leak and
# heap verdicts at teardown cover what the drop freed.
#
# A LEGITIMATE CLIENT CANNOT REACH THIS. OpenSSH queues its own non-kex
# output from its KEXINIT until its NEWKEYS (ssh_packet_send2), so the only
# replies a conforming client can make the server hold are to what it sent
# before it saw the server's KEXINIT -- bounded by the server's own window,
# a few small adjusts and replies. 6,000 opens behind its own KEXINIT is
# not something ssh(1) can be made to send.
if wants held-cap; then
    t0=$SECONDS
    m=$(mark)
    hc_open=$(hexof open-hex --type nosuch)
    peer held-cap "kexinit,flood:8000:90:$hc_open,rekey"
    settled "$m" held-cap
    hc_log="$TEST_TMPDIR/ssh-held-cap.log"
    echo "  held-cap: rc $PEER_RC, last line: $(tail -1 "$hc_log")"
    check "held-cap: the peer was cut off mid-exchange (rc 4, \`peer: closed\`)" \
        test "$PEER_RC" = 4 -a "$(grep -vxF 'peer: wrote 8000 x 90' "$hc_log")" \
            = "$(printf '%s\n' 'peer: authenticated' 'peer: sent 20 seq=2' \
                'peer: writing 8000 x 90 seqs=3..8002' 'peer: closed')"
    check "held-cap: the guest logged the bound, once, and then ended the connection" \
        test "$(since_re "$m" "${DBG}rekey: held output past 131072 bytes")" = 1 \
            -a "$(since "$m" | grep -xE -- "${DBG}(rekey: held output past 131072 bytes|disconnect: .*)" \
                  | sed -E 's/^.*: (rekey|disconnect): /\1: /' || true)" \
            = "$(printf '%s\n' 'rekey: held output past 131072 bytes' 'disconnect: protocol error')"
    check "held-cap: the exchange never completed" \
        test "$(since_re "$m" "$DONE_RE")" = 0
    took held-cap
fi

hostile_finish
