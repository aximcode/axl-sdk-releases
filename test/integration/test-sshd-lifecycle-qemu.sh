#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,tmux est=120 local-only=0 builds=sshd,console-device-hold guest=none
# guest=none: it types on the guest's USB keyboard (QMP send-key) and into
#   its Shell (test_send_stdin); a leased guest takes neither.
# test-sshd-lifecycle-qemu.sh -- the `sshd` tool's life around its sessions:
# the launcher's refusals and options, read-only sessions, a client that
# stops reading, `sshd stop` with sessions open, a typed `exit`, and a second
# console take-over. What a session sees while it runs is test-sshd-qemu.sh.
#
# The host types into the Shell over the serial console (test_enable_stdin).
# The run's authorized_keys is at \keys\ak, NOT at the root of sshd.efi's
# volume, so a start without -k or -P has no way to log in.
#
# Scenarios, in run order:
#   refusals        the launcher refuses, with its own line and deploying
#                   nothing: no way to log in (no -k, no default file, no
#                   -P); a -k file it cannot read; more than 8 -x ranges
#   explicit-k      `sshd -k \keys\ak` serves, says it found no sshd.conf
#                   (naming the path beside sshd.efi), and the key logs in
#   already-running a second `sshd` says the running server was not
#                   reconfigured
#   exempt-second   `-x A -x B` crosses to the driver as one list and BOTH
#                   ranges apply: four failed logins at once from the host
#                   (B) would activate a penalty that refuses the fifth
#                   connection, and with B exempt the fifth logs in
#   readonly        `sshd -R`: the launcher says (read-only); what a session
#                   types never reaches the console -- not even the Shell's
#                   echo -- while the console's output still reaches it
#   stalled-client  ssh-peer.py opens a shell with a 2 KB window and stops
#                   reading, then `type` of a file whose VT stream is far past
#                   a channel's 16 KB runs: a real ssh(1) session receives
#                   the trailing sentinel line, the serial console shows it,
#                   and sshd logs (DEBUG) that a session fell behind. When the
#                   stalled client reads again it gets what was queued -- at
#                   most the window plus the channel's 16 KB -- and then a
#                   repaint (`ESC[2J`) holding the sentinel; nothing it missed
#                   is replayed
#   stop-sessions   two sessions open, `sshd stop`: both ssh(1) exit, the
#                   serial console still runs commands, the driver's unload
#                   prints one clean leak verdict, and `sshd` starts again
#   remote-exit     two sessions open, one types `exit`: both receive the
#                   goodbye exactly once and close, the driver releases the
#                   console; a third, a tmux terminal 20 columns and 10 rows
#                   larger than the console (so confined to a scroll region), shows the
#                   goodbye as exactly its own line BELOW the console's last
#                   line -- not on row 2 over the console's text, where
#                   resetting the region homed the cursor; in the next Shell (Continue from the boot
#                   manager) typing faults nothing, `sshd status` says the
#                   server lost its Shell, and `sshd` replaces it and serves
#   conflict        a console-device fixture driver holds the take-over:
#                   `sshd` refuses with its message and status 1, and
#                   deploys nothing
#
# Usage: ./test/integration/test-sshd-lifecycle-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/sshd-tmux-lib.sh"

# The sessions are real ones; remote-exit's larger terminal is tmux's.
sshd_require_tools ssh ssh-keygen tmux
if ! tmux_probe; then
    missing_tool "tmux at $(command -v tmux) cannot start a server and capture its pane"
fi

test_parse_args "$@"
test_setup

PW=p4f-demo-pw
# A documentation range (RFC 5737) first: the host is in the SECOND range.
EXEMPT="-x 192.0.2.0/24 -x $HOST_AS_SEEN"
GOODBYE="[sshd] the UEFI Shell exited; console released"
# The channel limit sshd sets, and the stalled peer's window.
CHAN_LIMIT=16384
STALL_WINDOW=2048
# Lines of the file `type` prints while the peer is stalled.
BIG_N=120

sshd_build
mkdir -p "$TEST_STAGING/keys"
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/keys/ak"
for i in $(seq -w 1 "$BIG_N"); do
    printf 'big-%s-abcdefghijklmnopqrstuvwxyz0123456789\r\n' "$i"
done > "$TEST_STAGING/big.txt"
BIG_LAST="big-$(seq -w 1 "$BIG_N" | tail -1)-abcdefghijklmnopqrstuvwxyz0123456789"

TEST_BUILD_DIR="$(test_build_dir)"
declare -A _HOLD_ARCH=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_HOLD_ARCH[$TEST_ARCH]}" console-device-hold \
    2>&1 | tail -1
test_add_efi "$TEST_BUILD_DIR/drivers/console-device-hold.efi" console-device-hold.efi

echo "=== sshd lifecycle ($TEST_ARCH) ==="
sshd_boot ""

RAND=$(rand_tag)
KEYOPT='-k \\keys\\ak'

# --- refusals -----------------------------------------------------------------
send 'sshd\r'
check "refusals: no -k, no default file and no -P is refused" \
    wait_guest 'sshd: no way to log in - give -k with an authorized_keys file, put one at FS0:\authorized_keys, or give -P' 1 20
send 'sshd -k \\nokeys\r'
check "refusals: a -k file that is not there is refused" \
    wait_guest 'sshd: cannot read FS0:\nokeys' 1 20
send 'sshd -P x -x 1.0.0.1/32 -x 1.0.0.2/32 -x 1.0.0.3/32 -x 1.0.0.4/32 -x 1.0.0.5/32 -x 1.0.0.6/32 -x 1.0.0.7/32 -x 1.0.0.8/32 -x 1.0.0.9/32\r'
check "refusals: a ninth -x range is refused" \
    wait_guest 'sshd: at most 8 -x ranges' 1 20
send 'sshd status\r'
check "refusals: none of them deployed anything" \
    wait_guest "sshd: stopped" 1 20

# --- explicit-k -----------------------------------------------------------------
send "sshd $SSHD_LOG_HERE $KEYOPT -P $PW $EXEMPT\\r"
check "explicit-k: \`sshd -k\` serves" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 1 60
check "explicit-k: with no sshd.conf, the start names the file it looked for" \
    test "$(guest_lines 'sshd: no FS0:\sshd.conf; using defaults')" = 1
sshd_read_geometry || true
ssh_open K
ssh_wait_repaint K 40 || true
ssh_type K "echo %T%-k$RAND\\r"
check "explicit-k: the key in the -k file logs in, and the session runs a command" \
    wait_session "$(ssh_out K)" "p4f-k$RAND" 1 30
ssh_close K

# --- already-running ----------------------------------------------------------
send "sshd -P other\\r"
check "already-running: a second \`sshd\` leaves the running server as it is" \
    wait_guest "sshd: already running - the running server was not reconfigured; run 'sshd stop' then 'sshd' again to change its options" 1 20

# --- exempt-second --------------------------------------------------------------
# Four failed logins together accrue 4 x 5 s, past the 15 s at which a
# penalty activates (sshd's defaults): unless the host's range is exempt, the
# fifth connection is refused at accept.
for i in 1 2 3 4; do
    peer_start "bad$i" "wrong-$i" ""
done
for i in 1 2 3 4; do
    wait "${PEER_PID[bad$i]}" 2>/dev/null || true
    PEER_PID[bad$i]=""
done
nbad=0
for i in 1 2 3 4; do peer_has "bad$i" "peer: auth failed" && nbad=$((nbad + 1)); done
check "exempt-second: four wrong passwords are each refused ($nbad)" test "$nbad" = 4
peer_start good "$PW" "hold:1"
wait "${PEER_PID[good]}" 2>/dev/null || true
PEER_PID[good]=""
check "exempt-second: the fifth connection logs in -- the second -x range is exempt" \
    peer_has good "peer: authenticated"

# --- readonly -------------------------------------------------------------------
send 'sshd stop\r'
wait_guest "sshd: stopped" 2 30 || true
send "sshd -R $SSHD_LOG_HERE $KEYOPT $EXEMPT\\r"
check "readonly: the launcher says the server is read-only" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT (read-only)" 1 60
ssh_open R
ssh_wait_repaint R 40 || true
sleep 1
ssh_type R "echo %T%-ro$RAND\\r"
send "echo %T%-rw$RAND\\r"
check "readonly: the console's output still reaches the session" \
    wait_session "$(ssh_out R)" "p4f-rw$RAND" 1 30
sleep 2
test_clean_log
n=$(grep -acF -- "ro$RAND" "$TEST_CLEAN_LOG" || true)
check "readonly: what the session typed never reached the console (${n:-0} serial lines)" \
    test "${n:-0}" = 0
ssh_close R
send 'sshd stop\r'
wait_guest "sshd: stopped" 3 30 || true

# --- stalled-client ---------------------------------------------------------------
# DEBUG for the sshd domain, read by the new driver image at its first log line.
send 'set AXL_LOG_LEVEL sshd:debug\r'
send "sshd $SSHD_LOG_HERE $KEYOPT -P $PW $EXEMPT\\r"
wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 2 60 || true
peer_start S "$PW" "send:90:$(peer_open $STALL_WINDOW),expect:91,send:98:$(peer_req shell 1),expect:99,await:$TEST_TMPDIR/s.go,send:93:$(peer_adjust 4000000),expectdata:100000000"
wait_peer_line S "peer: got 99" 60 || true
ssh_open W
ssh_wait_repaint W 40 || true
sleep 1
behind0=$(guest_re "${DBG_PFX}a session fell behind \\(its client is not reading\\); it will be repainted \\([0-9]+ more since the last report\\)\$")
send 'type big.txt\r'
send "echo %T%-sent$RAND\\r"
check "stalled-client: a live session receives the sentinel after the burst" \
    wait_session "$(ssh_out W)" "p4f-sent$RAND" 1 60
check "stalled-client: the serial console shows it" \
    wait_guest "p4f-sent$RAND" 1 20
n=$(guest_re "${DBG_PFX}a session fell behind \\(its client is not reading\\); it will be repainted \\([0-9]+ more since the last report\\)\$")
check "stalled-client: sshd logs that a session fell behind ($behind0 -> $n)" \
    test "$n" -gt "$behind0"
check "stalled-client: the stalled client is still connected" \
    kill -0 "${PEER_PID[S]}"
# Output has stopped: the stalled client now reads.
sleep 1
touch "$TEST_TMPDIR/s.go"
for _i in $(seq 1 150); do
    peer_save S "$TEST_TMPDIR/S.out"
    (( $(session_count "$TEST_TMPDIR/S.out" "p4f-sent$RAND") >= 1 )) && break
    sleep 0.2
done
sleep 2
peer_stop S
stream=$(peer_stream S)
before=${stream%\\x1b\[2J*}
qlen=$(esc_len "$before")
peer_save S "$TEST_TMPDIR/S.out"
tail -c +$((qlen + 1)) "$TEST_TMPDIR/S.out" > "$TEST_TMPDIR/S.after"
clears=$(grep -aoF $'\e[2J' "$TEST_TMPDIR/S.out" | wc -l)
echo "  stalled-client: $clears clears; $qlen bytes before the last one"
check "stalled-client: the stalled session is repainted (a second clear: $clears)" \
    test "$clears" -ge 2
check "stalled-client: only the window plus the channel's limit came before the repaint ($qlen <= $((STALL_WINDOW + CHAN_LIMIT)))" \
    test "$qlen" -le $((STALL_WINDOW + CHAN_LIMIT))
n=$(session_count "$TEST_TMPDIR/S.out" "p4f-sent$RAND")
m=$(session_count "$TEST_TMPDIR/S.after" "p4f-sent$RAND")
check "stalled-client: the sentinel reaches it once, in the repaint ($n, $m after the last clear)" \
    test "$n" = 1 -a "$m" = 1
head -c "$qlen" "$TEST_TMPDIR/S.out" > "$TEST_TMPDIR/S.before"
n=$(session_count "$TEST_TMPDIR/S.before" "$BIG_LAST")
m=$(session_count "$TEST_TMPDIR/S.after" "$BIG_LAST")
check "stalled-client: the burst's last line is not replayed, only repainted ($n before the last clear, $m after)" \
    test "$n" = 0 -a "$m" = 1
ssh_close W

# --- stop-sessions ------------------------------------------------------------------
# R is LARGER than either console (x64 100x31, aa64 80x50): sshd confines it
# to the console's rows with a scroll region, which `sshd stop` must reset.
ssh_open P
ssh_open Q
ssh_open_sized R 132 60
ssh_wait_repaint P 40 || true
ssh_wait_repaint Q 40 || true
ssh_wait_repaint R 40 || true
sleep 1
send 'echo %T%-stop-begin\r'
wait_guest "p4f-stop-begin" 1 20 || true
stopped0=$(guest_lines "sshd: stopped")
send 'sshd stop\r'
check "stop-sessions: \`sshd stop\` with two sessions open prints 'sshd: stopped'" \
    wait_guest "sshd: stopped" $((stopped0 + 1)) 30
check "stop-sessions: session P's ssh(1) exits" ssh_wait_exit P 20
check "stop-sessions: session Q's ssh(1) exits" ssh_wait_exit Q 20
check "stop-sessions: session R's ssh(1) exits" ssh_wait_exit R 20
n=$(session_raw_count R '\x1b\[1;\d+r')
check "stop-sessions: the larger session R was confined to a scroll region ($n)" test "$n" -ge 1
n=$(session_raw_count R '\x1b7\x1b\[r\x1b8\r\n\[sshd\] stopped\x1b\[K\r\n')
check "stop-sessions: R's region is reset, cursor kept, before the last word, once ($n)" test "$n" = 1
n1=$(session_raw_count P '(?<!\x1b8)\r\n\[sshd\] stopped\x1b\[K\r\n')
n2=$(session_raw_count P '\x1b\[r')
check "stop-sessions: P (no region) is told, with no region reset ($n1, $n2)" \
    test "$n1" = 1 -a "$n2" = 0
send "echo %T%-local$RAND\\r"
check "stop-sessions: the local console still runs commands" \
    wait_guest "p4f-local$RAND" 1 20
test_clean_log
window=$(awk '$0 == "p4f-stop-begin" { on = 1; next }
              on && $0 == "sshd: stopped" { exit }
              on { print }' "$TEST_CLEAN_LOG")
n_ok=$(grep -cE "${LOG_PFX}mem: no leaks detected\$" <<< "$window" || true)
n_leak=$(grep -cF "$TEST_LEAK_MARKER" <<< "$window" || true)
check "stop-sessions: the driver's own unload prints one clean leak verdict (clean=$n_ok leak=$n_leak)" \
    test "$n_ok" = 1 -a "$n_leak" = 0
send 'set -d AXL_LOG_LEVEL\r'
serving0=$(guest_lines "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT")
send "sshd $SSHD_LOG_HERE $KEYOPT -P $PW $EXEMPT\\r"
check "stop-sessions: \`sshd\` starts again cleanly" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" $((serving0 + 1)) 60

# --- remote-exit --------------------------------------------------------------------
ssh_open E1
ssh_open E2
# remain-on-exit keeps the pane -- and the screen the goodbye left -- after
# ssh(1) exits.
# Larger than the console both ways (x64's is 100x31, aa64's 80x50), so the
# session is confined to a scroll region.
E3_COLS=$(( ${COLS:-112} + 20 )) E3_ROWS=$(( ${ROWS:-50} + 10 ))
tmux_ssh_open_sized E3 "$E3_COLS" "$E3_ROWS" 'set -g remain-on-exit on'
ssh_wait_repaint E1 40 || true
ssh_wait_repaint E2 40 || true
send "echo %T%-e3$RAND\\r"
tmux_wait_screen E3 "p4f-e3$RAND" 40 || true
sleep 1
ssh_type E1 'exit\r'
check "remote-exit: the driver releases the console when the Shell exits" \
    wait_guest_re "${LOG_PFX}sshd: the UEFI Shell exited; console released\$" 1 30
check "remote-exit: the session that typed exit is told, and closes" ssh_wait_exit E1 20
check "remote-exit: so is the other session" ssh_wait_exit E2 20
n1=$(session_count "$(ssh_out E1)" "$GOODBYE")
n2=$(session_count "$(ssh_out E2)" "$GOODBYE")
check "remote-exit: each session received the goodbye exactly once ($n1, $n2)" \
    test "$n1" = 1 -a "$n2" = 1
tmux_wait_screen E3 "$GOODBYE" 20 || true
E3_SCREEN=$(tmux_screen E3 2>/dev/null || true)
printf '%s\n' "$E3_SCREEN" > "$TEST_TMPDIR/E3.screen"
# Rows are 1-based. The console's last line is the Shell's prompt the
# `exit` was typed at (`FS0:\>`, with or without the echoed `exit`: the
# Shell may be gone before its echo is flushed); the goodbye goes on a line
# of its own below it.
n=$(grep -cF -- '[sshd]' <<< "$E3_SCREEN" || true)
row_bye=$(awk -v w="$GOODBYE" '$0 == w { print NR; exit }' <<< "$E3_SCREEN")
row_prompt=$(awk '/^FS[0-9]+:.*>( ?exit)?$/ { r = NR } END { if (r) print r }' <<< "$E3_SCREEN")
check "remote-exit: the ${E3_COLS}x${E3_ROWS} terminal shows one goodbye, exactly its own line ($n, row ${row_bye:-none})" \
    test "$n" = 1 -a -n "$row_bye"
check "remote-exit: and BELOW the console's last line (goodbye row ${row_bye:-none}, prompt row ${row_prompt:-none})" \
    test -n "$row_bye" -a -n "$row_prompt" -a "${row_bye:-0}" -gt "${row_prompt:-99}"
if [[ $FAIL -gt 0 ]]; then
    echo "--- E3 (${E3_COLS}x${E3_ROWS}) screen ---"; cat -A "$TEST_TMPDIR/E3.screen" | head -40
fi
tmux_close E3
# The boot manager's front page, highlight on its first entry: four Downs
# reach Continue, which boots the Shell again.
wait_guest_re 'Boot Maintenance Manager' 1 30 || true
sleep 3
for _i in 1 2 3 4; do send '\e[B'; sleep 0.5; done
send '\r'
check "remote-exit: a fresh Shell is back at its prompt" \
    wait_guest "SSHD-BOOTED" 2 90
sleep 1
send 'set -v T p4f\r'
send 'sshd status\r'
check "remote-exit: \`sshd status\` says the server lost its Shell" \
    wait_guest "sshd: stopped (the Shell it served exited)" 1 20
# A keyboard the take-over evicted, still holding the exited Shell's notifies,
# calls freed code on a key that matches one: TerminalDxe's IsKeyRegistered
# compares only scan code and character, so a plain 's' (in `sshd status`)
# matches its Ctrl-S notify and a plain 'c' its Ctrl-C break. Type both.
send 'echo %T%-c-typed\r'
wait_guest "p4f-c-typed" 1 20 || true
n=$(cpu_exceptions)
check "remote-exit: typing at the new Shell faults nothing ($n CPU exceptions)" \
    test "$n" = 0
serving0=$(guest_lines "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT")
send "sshd $SSHD_LOG_HERE $KEYOPT -P $PW $EXEMPT\\r"
check "remote-exit: \`sshd\` replaces the server left behind and serves the new Shell" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" $((serving0 + 1)) 60
ssh_open N
ssh_wait_repaint N 40 || true
ssh_type N "echo %T%-new$RAND\\r"
check "remote-exit: a session on the new Shell runs a command" \
    wait_session "$(ssh_out N)" "p4f-new$RAND" 1 30
ssh_close N
stopped0=$(guest_lines "sshd: stopped")
send 'sshd stop\r'
wait_guest "sshd: stopped" $((stopped0 + 1)) 30 || true

# --- conflict -----------------------------------------------------------------------
send 'load console-device-hold.efi\r'
check "conflict: the fixture driver holds the console take-over" \
    wait_guest "HOLD-READY" 1 30
send "sshd -P $PW\\r"
check "conflict: \`sshd\` refuses with its message" \
    wait_guest "sshd: another console take-over is active (fbcon or a BMC application)" 1 30
send 'echo %T%-rc%lasterror%\r'
check "conflict: and exits with status 1" wait_guest "p4f-rc0x1" 1 20
stopped0=$(guest_lines "sshd: stopped")
send 'sshd status\r'
check "conflict: nothing was deployed" wait_guest "sshd: stopped" $((stopped0 + 1)) 20
send 'echo cdh-release\r'
wait_guest "HOLD-RELEASED" 1 20 || true

sshd_poweroff
n=$(heap_corruptions)
check "no heap corruption reported in the run ($n)" test "$n" = 0
test_clean_log
if test_check_leaks; then
    pass "leak gate: every verdict in the run is clean"
else
    fail "leak gate: a leak or a missing verdict"
fi

echo ""
printf "sshd-lifecycle: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 48 ]] && exit 0 || exit 1
