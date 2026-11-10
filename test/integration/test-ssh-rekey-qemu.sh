#!/bin/bash
# test-meta: arch=both needs=ssh est=51 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-rekey-qemu.sh -- AxlSsh key re-exchange in an authenticated
# session, driven by a real ssh(1): the client starting it
# (`-o RekeyLimit=32K`), the server starting it (rekeylimit=65536, the
# fixture's axl_ssh_server_set_rekey_limit), output, input and an
# interactive shell flowing across every exchange byte-exact, and a KEXINIT
# before authentication still ending the connection.
#
# The guest runs ssh-server-selftest.efi with `channels=1` (the fixture
# shell) and `sshloglevel=debug`, because every rekey line the server writes
# is DEBUG -- sshd logs rekeying at debug level only. The setup, helpers and
# teardown checks are lib/ssh-hostile-lib.sh's: the boot, the summed
# deadline, the heartbeat, one principal release per successful login, the
# heap-corruption verdict, the re-entry tripwire and the leak verdict.
#
# ONE SERVER, rekeylimit=65536 for the whole boot. The client scenarios set
# RekeyLimit=32K, half of it, so the CLIENT's count crosses first and starts
# each of their exchanges ("rekey: started by the peer"); the server
# scenarios set none, so ssh(1)'s own limit (1 GiB) is never reached and
# every exchange there is the server's ("rekey: started by us").
#
# Scenarios, in run order:
#   client-rekey-interactive
#                    `ssh -tt -o RekeyLimit=32K` into the line shell, 256 KB
#                    of `echo N` lines in 32 KB chunks, every echo read back
#                    byte-exact, `exit 7`
#   alloc-across-rekey
#                    the same shell: the allocator's live blocks and bytes
#                    (the shell's `mem`) at a quiet point after the first
#                    rekeys and again after several more -- equal, so an
#                    exchange allocates nothing it keeps (one payload buffer
#                    for the connection's life)
#   client-rekey-bulk
#                    `ssh -o RekeyLimit=32K host 'stream 1048576'`: 1 MiB of
#                    a non-periodic pattern, SHA-256 exact, rc 0
#   server-rekey     `ssh host 'stream 524288'`: at least four exchanges the
#                    SERVER started, output exact, rc 0
#   server-rekey-stdin
#                    `ssh host cat` with 192 KB in: echoed exact; the inbound
#                    count starts an exchange too
#   auth-rekey-refused
#                    ssh-peer.py sends KEXINIT after key exchange and before
#                    USERAUTH_REQUEST: the server ends the connection
#                    (deviation 1, spec section 12a.7)
#
# Every ssh(1) string pinned below was MEASURED from a real transcript
# (OpenSSH 9.9p1), not assumed. Transcripts: SSH_HOSTILE_KEEP=<dir>.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-rekey-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh rekey"
HOSTILE_SCENARIOS="client-rekey-interactive alloc-across-rekey client-rekey-bulk server-rekey server-rekey-stdin auth-rekey-refused"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh). Measured 2026-10-09, standalone: interactive 5,
# alloc 3, bulk 2, server-rekey 1, stdin 1, auth 0 on aa64 (TCG), and the
# same to within a second on x64 -- the scenarios are bound by ssh(1) and
# the fixed waits, not by the guest. Each budget is about three times that,
# for a loaded pool.
declare -A HOSTILE_BUDGET=(
    [client-rekey-interactive]=15 [alloc-across-rekey]=10
    [client-rekey-bulk]=8 [server-rekey]=6 [server-rekey-stdin]=6
    [auth-rekey-refused]=4
)

if ! command -v ssh >/dev/null 2>&1; then
    echo "SKIP: no ssh(1) on the host -- every scenario here is a real session"
    exit 77
fi

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
REKEY_LIMIT=65536
HOSTILE_EXTRA_ARGS="channels=1 sshloglevel=debug rekeylimit=$REKEY_LIMIT"

hostile_boot "AxlSsh rekey"

PEER="$(dirname "$0")/ssh-peer.py"

# --- helpers ------------------------------------------------------------------

# A publickey login with the staged key, the developer's own keys and agent
# kept out (good_login_works' reasoning).
SSH_PK=(-p "$HOST_PORT" -o StrictHostKeyChecking=no
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes
        -o ConnectTimeout=20 -o IdentitiesOnly=yes -o IdentityAgent=none
        -o PreferredAuthentications=publickey -i "$TEST_TMPDIR/good")

# sess NAME [ssh args...] -- one ssh(1) run as root. Stdout to NAME.out,
# stderr to NAME.err, ssh's own -v log to ssh-NAME.log (where hostile_finish
# counts logins). Stdin is SESS_IN (default /dev/null), the remote command
# SESS_CMD when set. Sets SESS_RC.
sess() {
    local name="$1"
    shift
    SESS_RC=0
    timeout 90 ssh -v -E "$TEST_TMPDIR/ssh-$name.log" "${SSH_PK[@]}" \
        "$@" "root@127.0.0.1" ${SESS_CMD+"$SESS_CMD"} \
        > "$TEST_TMPDIR/$name.out" 2> "$TEST_TMPDIR/$name.err" \
        < "${SESS_IN:-/dev/null}" || SESS_RC=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$name.log")
}

# A scenario's wall time, for sizing HOSTILE_BUDGET from a real run.
took() { echo "  $1 took $((SECONDS - t0))s"; }

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

# The server's rekey lines, whole: DEBUG, so the console timestamp, the
# level and `ssh:func:line: ` come first (measured).
DBG='[0-9:.]+ \[DEBUG\] ssh:[a-z_0-9]+:[0-9]+: '
DONE_RE="${DBG}rekey: complete"
PEER_RE="${DBG}rekey: started by the peer"
OURS_RE="${DBG}rekey: started by us, [0-9]+ bytes out, [0-9]+ in"
CLOSE_RE='SERVER: CHANNEL-CLOSE ud=[0-9]+'

# How many of ssh(1)'s own "KEXINIT sent" lines in -v log $1 come AFTER its
# authentication -- one per exchange after login, whoever started it.
# MEASURED (OpenSSH 9.9p1): `debug1: SSH2_MSG_KEXINIT sent`, CRLF-ended in
# an -E log; the summary line `Authenticated to ...` marks login.
client_rekeys() {
    tr -d '\r' < "$1" \
        | awk '/^Authenticated to /{a=1} a && /^debug1: SSH2_MSG_KEXINIT sent$/{n++} END{print n+0}'
}

# The line shell's input for lines $1..$2 (`echo N` and CR each), and the
# exact output it must produce: the echo of the line, CRLF, then N and CRLF.
shell_in()  { seq "$1" "$2" | awk '{printf "echo %d\r", $1}'; }
shell_out() { seq "$1" "$2" | awk '{printf "echo %d\r\n%d\r\n", $1, $1}'; }

# Wait up to $3 s for file $1 to reach $2 bytes.
wait_size() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        if [[ "$(stat -c %s "$1" 2>/dev/null || echo 0)" -ge "$2" ]]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# drive_shell NAME CHUNK_LINES CHUNKS MEM_AFTER [ssh args...] -- one
# `ssh -tt` session into the line shell (the extra ssh args, such as
# `-o RekeyLimit=32K`, choose who starts the exchanges), fed CHUNKS chunks of
# CHUNK_LINES `echo N` lines through a FIFO. Each chunk waits for its echo
# to come back whole before the next is written, so the channel's output
# queue never holds more than one chunk's echo (the fixture's shell does not
# retry an AXL_BUSY write). After chunk MEM_AFTER and after the last, a quiet
# moment and `mem`; then `exit 7`. With DRIVE_FLOOD=N set, chunk 1 and the
# first `mem` are each followed by the shell's `flood N` (N bytes of 'x' in
# 4 KB writes, resumed from the writable hook): output-driven, so the
# server's own exchanges start on its send path and hold the packet that
# started them. Leaves NAME.expect beside NAME.out.
drive_shell() {
    local name="$1" per="$2" chunks="$3" mem_after="$4"
    shift 4
    local fifo="$TEST_TMPDIR/$name.fifo" out="$TEST_TMPDIR/$name.out"
    local exp="$TEST_TMPDIR/$name.expect" m0
    mkfifo "$fifo"
    : > "$exp"
    m0=$(mark)
    {
        local k lo hi
        for ((k = 1; k <= chunks; k++)); do
            lo=$(( (k - 1) * per + 1 )); hi=$(( k * per ))
            shell_in "$lo" "$hi"
            shell_out "$lo" "$hi" >> "$exp"
            wait_size "$out" "$(stat -c %s "$exp")" 60 || true
            if [[ "$k" -eq "$mem_after" || "$k" -eq "$chunks" ]]; then
                sleep 1
                printf 'mem\r'
                printf 'mem\r\n' >> "$exp"
                wait_since "$m0" "$(( k == chunks ? 2 : 1 ))" 20 \
                    'SERVER: MEM [0-9]+ [0-9]+' || true
            fi
            if [[ -n "${DRIVE_FLOOD:-}" && ( "$k" -eq 1 || "$k" -eq "$mem_after" ) ]]; then
                printf 'flood %s\r' "$DRIVE_FLOOD"
                {
                    printf 'flood %s\r\n' "$DRIVE_FLOOD"
                    head -c "$DRIVE_FLOOD" /dev/zero | tr '\0' x
                } >> "$exp"
                wait_size "$out" "$(stat -c %s "$exp")" 60 || true
            fi
        done
        printf 'exit 7\r'
        printf 'exit 7\r\n' >> "$exp"
    } > "$fifo" &
    local feed=$!
    BG_PIDS+=("$feed")
    TERM=xterm SESS_IN="$fifo" sess "$name" -tt "$@"
    wait "$feed" 2>/dev/null || true
}

# rekey_audit MARK -- the order of the server's rekey lines after MARK,
# against the moment the server queued its CLOSE for the channel (`chan N:
# our CLOSE queued`). Prints "OPEN TAIL INDONE":
#   OPEN    exchanges started while the channel was still open that were
#           never followed by "rekey: complete" before the next one started
#           -- must be 0;
#   TAIL    the same for an exchange started after our CLOSE was queued,
#           which can only be the last: the final CLOSE is counted after its
#           push and can carry the outbound count over the limit, the
#           client's next packet then meets pump's read-side check (sshd
#           checks the same place, ssh_packet_read_poll2 ->
#           ssh_packet_check_rekey, whose conditions do not ask whether the
#           connection is closing), and the client goes before answering --
#           at most 1;
#   INDONE  exchanges the server started on its INBOUND count (`... I in`
#           with I over REKEY_LIMIT) that completed.
rekey_audit() {
    since "$1" | awk -v l="$REKEY_LIMIT" '
        / rekey: started by / {
            if (pend) { if (popen) open++; else tail++ }
            pend = 1; popen = !closed; pin = ($NF == "in" && $(NF-1) > l)
        }
        / rekey: complete$/ { if (pend) { if (pin) indone++; pend = 0 } }
        / chan [0-9]+: our CLOSE queued$/ { closed = 1 }
        END {
            if (pend) { if (popen) open++; else tail++ }
            print open + 0, tail + 0, indone + 0
        }'
}

# The two `mem` readings after mark $1, as "COUNT BYTES" lines.
mem_lines() { since "$1" | grep -xE 'SERVER: MEM [0-9]+ [0-9]+' | cut -d' ' -f3- || true; }

# --- client-rekey-interactive ------------------------------------------------
#
# 24000 lines (~250 KB in, ~430 KB out) in eight 3000-line chunks, so each
# chunk crosses the client's 32 KB limit at least once in each direction.
# The whole transcript is compared by SHA-256 against the one the shell must
# have produced: a byte lost, repeated or reordered across an exchange
# changes it.
if wants client-rekey-interactive; then
    t0=$SECONDS
    m=$(mark)
    drive_shell interactive 3000 8 4 -o RekeyLimit=32K
    wait_since "$m" 1 15 "$CLOSE_RE" || true
    ci_client=$(client_rekeys "$TEST_TMPDIR/ssh-interactive.log")
    ci_done=$(since_re "$m" "$DONE_RE")
    ci_peer=$(since_re "$m" "$PEER_RE")
    echo "  interactive: ssh sent $ci_client KEXINIT(s) after login;" \
         "the server completed $ci_done exchange(s), $ci_peer started by ssh"
    check "client-rekey-interactive: ssh -tt exits with the shell's status, 7" \
        test "$SESS_RC" = 7
    check "client-rekey-interactive: every echo came back byte-exact (SHA-256 of $(stat -c %s "$TEST_TMPDIR/interactive.expect") bytes)" \
        test "$(sha256sum < "$TEST_TMPDIR/interactive.out")" \
            = "$(sha256sum < "$TEST_TMPDIR/interactive.expect")"
    check "client-rekey-interactive: ssh started at least 3 exchanges after login ($ci_client)" \
        test "$ci_client" -ge 3
    check "client-rekey-interactive: the server completed exactly as many as ssh sent ($ci_done)" \
        test "$ci_done" = "$ci_client"
    check "client-rekey-interactive: ...and ssh, not the server, started them ($ci_peer)" \
        test "$ci_peer" = "$ci_done"
    check "client-rekey-interactive: no fixture write was refused or failed" \
        test "$(since "$m" | grep -c 'SERVER: WRITE-FAIL' || true)" = 0
    took client-rekey-interactive
fi

# --- alloc-across-rekey -------------------------------------------------------
#
# Spec section 12a.4's "no second payload buffer": the allocator's live
# blocks and bytes at a quiet point after the first exchanges, and again
# after several more on the same connection, must be EQUAL. A rekey that
# allocated anything it kept -- a second 35,000-byte receive buffer, a key
# copy, a held queue per exchange -- shows here as a difference.
#
# The SERVER starts the exchanges between the readings: no client
# RekeyLimit, and a 512 KB `flood` carries its outbound count over 64 KB
# again and again from the send path, so the packet whose sending starts
# each one is HELD and sealed after our NEWKEYS. The held path runs between
# the readings -- required, not assumed. The same flood runs once BEFORE
# the first reading too, so the held queue (allocated with the connection,
# grown to its working size by the largest packet held) is at that size
# before either reading: what is compared is steady state, not warm-up.
if wants alloc-across-rekey; then
    t0=$SECONDS
    m=$(mark)
    DRIVE_FLOOD=524288 drive_shell alloc 1500 4 2
    wait_since "$m" 1 15 "$CLOSE_RE" || true
    mapfile -t al_mem < <(mem_lines "$m")
    # The exchanges completed BETWEEN the two readings: the lines after the
    # first MEM line and before the second.
    al_at=$(since "$m" | grep -nxE 'SERVER: MEM [0-9]+ [0-9]+' | cut -d: -f1 \
        | tr '\n' ' ' || true)
    read -r al_a al_b _ <<< "${al_at:-0 0} 0 0"
    al_between=$(since "$m" | sed -n "${al_a},${al_b}p" \
        | grep -cxE -- "$DONE_RE" || true)
    al_held=$(since "$m" | sed -n "${al_a},${al_b}p" \
        | grep -cxE -- "${DBG}rekey: sealed [1-9][0-9]* held after our NEWKEYS" \
        || true)
    echo "  alloc: mem ${al_mem[0]:-?} then ${al_mem[1]:-?}," \
         "$al_between exchange(s) between, $al_held of them holding output"
    check "alloc-across-rekey PRECONDITION: both readings were taken, after at least one exchange" \
        test "${#al_mem[@]}" = 2 -a "$(since_re "$m" "$DONE_RE")" -ge 1
    check "alloc-across-rekey PRECONDITION: at least 3 exchanges completed between the readings ($al_between)" \
        test "$al_between" -ge 3
    check "alloc-across-rekey PRECONDITION: the server started them and held output in at least one ($al_held)" \
        test "$al_held" -ge 1 -a "$(since_re "$m" "$PEER_RE")" = 0
    check "alloc-across-rekey: live blocks and bytes are equal across them (${al_mem[0]:-?} / ${al_mem[1]:-?})" \
        test -n "${al_mem[0]:-}" -a "${al_mem[0]:-x}" = "${al_mem[1]:-y}"
    check "alloc-across-rekey: the shell exited 7 with its transcript exact" \
        test "$SESS_RC" = 7 -a "$(sha256sum < "$TEST_TMPDIR/alloc.out")" \
            = "$(sha256sum < "$TEST_TMPDIR/alloc.expect")"
    took alloc-across-rekey
fi

# --- client-rekey-bulk --------------------------------------------------------
if wants client-rekey-bulk; then
    t0=$SECONDS
    stream_file 1048576 "$TEST_TMPDIR/bulk.expect"
    m=$(mark)
    SESS_CMD='stream 1048576' sess bulk -o RekeyLimit=32K
    wait_since "$m" 1 15 "$CLOSE_RE" || true
    cb_client=$(client_rekeys "$TEST_TMPDIR/ssh-bulk.log")
    cb_done=$(since_re "$m" "$DONE_RE")
    echo "  bulk: ssh sent $cb_client KEXINIT(s) after login; the server completed $cb_done"
    check "client-rekey-bulk: ssh host 'stream 1048576' exits 0" \
        test "$SESS_RC" = 0
    check "client-rekey-bulk: exactly 1 MiB of the pattern arrived (SHA-256)" \
        test "$(sha256sum < "$TEST_TMPDIR/bulk.out")" \
            = "$(sha256sum < "$TEST_TMPDIR/bulk.expect")"
    read -r cb_open cb_tail _ <<< "$(rekey_audit "$m")"
    check "client-rekey-bulk: the output crossed at least one exchange ($cb_done), as many as ssh sent but one started after our CLOSE ($cb_client, $cb_tail)" \
        test "$cb_done" -ge 1 -a "$cb_client" = "$((cb_done + cb_tail))"
    check "client-rekey-bulk: every exchange started while the channel was open completed ($cb_open unfinished, $cb_tail after our CLOSE)" \
        test "$cb_open" = 0 -a "$cb_tail" -le 1
    check "client-rekey-bulk: the fixture queued all of it" \
        test "$(since_lines "$m" 'SERVER: STREAM-DONE 1048576')" = 1
    took client-rekey-bulk
fi

# --- server-rekey -------------------------------------------------------------
#
# 512 KB out against the server's 64 KB limit: the SERVER starts its
# exchanges, which ssh(1) answers. Each is counted from the guest's own line.
# Every exchange started while the channel was open must complete
# (rekey_audit). One more may start after the server has queued its CLOSE --
# the last packet out carries the count over and the client's reply meets
# the read-side check -- and the client is gone before it answers
# (measured: 8 started, 7 completed, 7 answered); rekey_audit accepts that
# one only as the last line, after the CLOSE. An exchange that the server
# STARTS from its send path holds the packet whose sending started it, and
# seals it after its NEWKEYS: at least one of those is required, so the hold
# is exercised, not just compiled.
if wants server-rekey; then
    t0=$SECONDS
    stream_file 524288 "$TEST_TMPDIR/srv.expect"
    m=$(mark)
    SESS_CMD='stream 524288' sess srv
    wait_since "$m" 1 15 "$CLOSE_RE" || true
    sr_ours=$(since_re "$m" "$OURS_RE")
    sr_done=$(since_re "$m" "$DONE_RE")
    sr_client=$(client_rekeys "$TEST_TMPDIR/ssh-srv.log")
    echo "  server-rekey: the server started $sr_ours, completed $sr_done; ssh answered $sr_client"
    check "server-rekey: ssh host 'stream 524288' exits 0 with the output exact (SHA-256)" \
        test "$SESS_RC" = 0 -a "$(sha256sum < "$TEST_TMPDIR/srv.out")" \
            = "$(sha256sum < "$TEST_TMPDIR/srv.expect")"
    check "server-rekey: the server started at least 4 exchanges ($sr_ours)" \
        test "$sr_ours" -ge 4
    read -r sr_open sr_tail _ <<< "$(rekey_audit "$m")"
    check "server-rekey: every exchange started while the channel was open completed ($sr_open unfinished)" \
        test "$sr_open" = 0
    check "server-rekey: at most one, started after our CLOSE, was left unanswered ($sr_tail), none started by ssh" \
        test "$sr_tail" -le 1 -a "$sr_done" = "$((sr_ours - sr_tail))" \
            -a "$(since_re "$m" "$PEER_RE")" = 0
    check "server-rekey: ssh answered each completed one with its own KEXINIT ($sr_client)" \
        test "$sr_client" = "$sr_done"
    check "server-rekey: an exchange held a packet and sealed it after our NEWKEYS ($(since_re "$m" "${DBG}rekey: sealed [1-9][0-9]* held after our NEWKEYS"))" \
        test "$(since_re "$m" "${DBG}rekey: sealed [1-9][0-9]* held after our NEWKEYS")" -ge 1
    took server-rekey
fi

# --- server-rekey-stdin -------------------------------------------------------
#
# 192 KB through `cat`, both directions past the 64 KB limit. 192 KB rather
# than 256: the fixture's cat writes each data hook's bytes straight back
# without retrying an AXL_BUSY, and the per-channel write limit is 256 KB, so
# an input that size could fill the queue while an exchange holds output.
if wants server-rekey-stdin; then
    t0=$SECONDS
    head -c 196608 /dev/urandom > "$TEST_TMPDIR/stdin.in"
    m=$(mark)
    SESS_IN="$TEST_TMPDIR/stdin.in" SESS_CMD=cat sess stdin
    wait_since "$m" 1 15 "$CLOSE_RE" || true
    ss_ours=$(since_re "$m" "$OURS_RE")
    # The inbound count past the limit, on a line the server wrote when it
    # started an exchange: " B bytes out, I in" with I > the limit.
    ss_in=$( { since "$m" | grep -oE 'started by us, [0-9]+ bytes out, [0-9]+ in$' \
        || true; } | awk -v l="$REKEY_LIMIT" '$7 > l {n++} END{print n+0}')
    echo "  stdin: the server started $ss_ours exchange(s), $ss_in on the inbound count"
    check "server-rekey-stdin: 196608 bytes through cat came back exact; rc 0" \
        test "$SESS_RC" = 0 -a "$(sha256sum < "$TEST_TMPDIR/stdin.out")" \
            = "$(sha256sum < "$TEST_TMPDIR/stdin.in")"
    read -r ss_open ss_tail ss_indone <<< "$(rekey_audit "$m")"
    check "server-rekey-stdin: the server started at least 2 exchanges ($ss_ours)" \
        test "$ss_ours" -ge 2
    check "server-rekey-stdin: every exchange started while the channel was open completed ($ss_open unfinished)" \
        test "$ss_open" = 0 -a "$ss_tail" -le 1
    check "server-rekey-stdin: an exchange the INBOUND count started COMPLETED ($ss_indone)" \
        test "$ss_indone" -ge 1
    check "server-rekey-stdin: the INBOUND count started at least one ($ss_in)" \
        test "$ss_in" -ge 1
    check "server-rekey-stdin: no fixture write was refused or failed" \
        test "$(since "$m" | grep -c 'SERVER: WRITE-FAIL' || true)" = 0
    took server-rekey-stdin
fi

# --- auth-rekey-refused -------------------------------------------------------
#
# Deviation 1 (spec section 12a.7), unchanged by rekey: a KEXINIT after key
# exchange and before authentication ends the connection. The body is
# never parsed -- the refusal is on the message number -- so one byte does.
if wants auth-rekey-refused; then
    t0=$SECONDS
    m=$(mark)
    ar_rc=0
    timeout 30 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" --user pwuser \
        --password "$PW_OK" --preauth send:20:00 \
        > "$TEST_TMPDIR/ssh-preauth.log" 2>&1 || ar_rc=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-preauth.log")
    wait_since "$m" 1 10 "${DBG}disconnect: key re-exchange before authentication" || true
    check "auth-rekey-refused: the peer is cut off, never authenticated (rc 4)" \
        peer_is "$ar_rc" 4 "$TEST_TMPDIR/ssh-preauth.log" \
            'peer: sent 20 seq=0' 'peer: closed'
    check "auth-rekey-refused: the guest names the refusal" \
        test "$(since_re "$m" "${DBG}disconnect: key re-exchange before authentication")" = 1
    took auth-rekey-refused
fi

hostile_finish
