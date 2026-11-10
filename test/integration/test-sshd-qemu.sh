#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,tmux est=120 local-only=0 builds=sshd guest=none
# guest=none: it types on the guest's USB keyboard (QMP send-key) and into
#   its Shell (test_send_stdin); a leased guest takes neither.
# test-sshd-qemu.sh -- the `sshd` tool: the live UEFI Shell over SSH, as real
# ssh(1) sessions see it.
#
# The guest boots to the Shell prompt and the host TYPES into it over the
# serial console (test_enable_stdin), exactly as an operator would: `sshd`
# deploys its resident driver and returns to the prompt, and from then on the
# same Shell is reachable two ways at once -- the serial console, which keeps
# painting (the take-over is passthrough), and real ssh(1) sessions.
# Lifecycle (stop with sessions open, `exit`, read-only, a stalled client, a
# second take-over, the launcher's refusals) is test-sshd-lifecycle-qemu.sh.
#
# Scenarios, in run order:
#   status-before   `sshd status` before any start prints `sshd: stopped`
#   serial-paste-plain  the control: SQ_N command lines (SQ_N x ~76 keys, far
#                   more than the device's 63-key ring) typed on the serial
#                   console in ONE write, before any take-over -- the
#                   firmware's own queue keeps every key, so each line runs
#                   exactly once
#   ctrl-c-ahead-plain  the control: a Ctrl-C typed on the serial console
#                   behind AHEAD_N keys of type-ahead into a looping script
#                   (which never reads the console) breaks it -- the Shell's
#                   notify fires when the firmware queues the key, not when
#                   it is read
#   start           `sshd` prints its serving line and returns to the Shell;
#                   the driver logs its own serving line with the console size
#   serial-paste    the same paste with sshd running: the take-over evicts the
#                   firmware keyboard and its read timer moves keys into the
#                   device ring only while the ring has room, leaving the rest
#                   in the firmware's queue -- so every line still runs
#                   exactly once, as on the plain Shell
#   ctrl-c-ahead    the same Ctrl-C with sshd running: the device reads the
#                   evicted keyboard ahead of the ring and fires the notify on
#                   arrival, so it breaks the script as on the plain Shell
#   shell-command   session A (`ssh -tt`, the authorized_keys at the root of
#                   sshd.efi's volume -- the default with no -k): ssh(1)
#                   prints the console geometry from the pre-authentication
#                   banner, the session opens with the repaint, and a command
#                   typed into it runs in the live Shell -- its output line
#                   arrives in the session EXACTLY ONCE, and the serial
#                   console shows it once (the local console still paints)
#   stderr          `sshd -T -x bogus` typed in T (tmux, the console's size)
#                   after a page of output: its error line (StdErr) is on
#                   T's RENDERED screen exactly once, whole, with the next
#                   prompt below it; in A's stream and on the serial
#                   console exactly once; the control, `ls` of a missing
#                   file (the Shell's own error, on StdOut), likewise. And
#                   sshd's OWN log (A's "ssh: Accepted publickey" line) is
#                   not in the session, only on the serial log (with T's)
#   paste           a 200-character command line in ONE write runs exactly:
#                   the console's key ring holds 63 keys, so the rest waits in
#                   the session's queue
#   paste-lines     a several-KB paste of many command lines, one write, runs
#                   every line exactly once
#   idle            a connected session on a quiet console receives no bytes
#                   for IDLE_S seconds (nothing resent per tick)
#   two-sessions    session B joins; a command A runs shows its output line
#                   exactly once in B, and a command B types runs and shows
#                   in A exactly once: both type into, and watch, one console
#   late-joiner     A runs a marker; then C joins: ssh(1) prints the geometry
#                   banner once, C's first bytes are the repaint's clear, and
#                   the marker's output line is in C exactly once -- the
#                   current screen, no op applied twice
#   late-joiner-stream  D (ssh-peer.py) joins during a fast burst of output:
#                   its stream starts with the repaint's clear, it gets no
#                   line twice (the join flushes what is buffered to everyone
#                   before it snapshots) and the burst's end once
#   ctrl-c          a looping script (`stall` per iteration) is broken by
#                   Ctrl-C typed in A: the prompt is back within CTRL_C_S
#                   seconds (a command typed after it runs), the script never
#                   finished, and sshd is still running
#   ctrl-c-stall    the negative control: `stall 5000000` never polls the
#                   break flag, so a Ctrl-C does not cut it short -- the
#                   command typed after it runs only once the stall is over
#   ctrl-c-paste    Ctrl-C behind a paste longer than the key ring, into the
#                   looping script (which never reads the console): the
#                   chord does not wait behind the paste, its notify fires on
#                   the full ring, the script breaks, and the paste is then
#                   typed in at the prompt, whole
#   ctrl-c-serial   the same break typed on the LOCAL serial console: the
#                   take-over evicts the firmware keyboard, so the chord
#                   reaches the Shell only through the device's read timer
#                   re-injecting it into the device ring and firing the
#                   device's copy of the Shell's notify -- the prompt is back
#                   within CTRL_C_S seconds and the script never finished
#   split-input     ssh-peer.py (password) sends what ssh(1) never splits on
#                   purpose: a UTF-8 character and an escape sequence cut
#                   across packets, LF and CRLF line endings (a CRLF split
#                   across packets too), and a command longer than the key
#                   ring in ONE packet -- each arrives as the right keys
#   exec-refused    `ssh host cmd` is refused with ssh(1)'s own message and a
#                   nonzero status, and nothing reaches the console
#   status-running  `sshd status` prints `sshd: running on 10.0.2.15`
#   stop            `sshd stop` prints `sshd: stopped`, and the DRIVER's own
#                   unload prints a clean leak verdict -- the one verdict
#                   between the stop command and its `sshd: stopped` line
#   status-after    `sshd status` prints `sshd: stopped` again
#
# Usage: ./test/integration/test-sshd-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/sshd-tmux-lib.sh"

# The sessions are real ones; the stderr scenario's rendered screen is tmux's.
sshd_require_tools ssh ssh-keygen tmux
if ! tmux_probe; then
    missing_tool "tmux at $(command -v tmux) cannot start a server and capture its pane"
fi

test_parse_args "$@"
test_setup

# Seconds of quiet console with a session attached (the idle scenario).
IDLE_S=8
# The prompt is back this soon after a Ctrl-C (host seconds; one iteration
# of the script is 0.1 s of stall).
CTRL_C_S=3
# The negative control's stall, and the least time its prompt can take.
STALL_US=5000000
STALL_MIN_S=4.5
PW=p4f-demo-pw
# Command lines in each serial paste, and the tag that keeps their output
# lines apart from every other scenario's.
SQ_N=6
# Keys typed ahead of a Ctrl-C into a script that never reads them: more than
# the device ring (63), fewer than OVMF TerminalDxe's own queue (measured: a
# plain Shell breaks behind 100, not behind 150).
AHEAD_N=100
SQ_RAND=$(od -An -tx1 -N4 /dev/urandom | tr -d ' \n')
# Lines in the burst a session joins in the middle of. Long enough to
# outlast the join on a loaded host: the join point is set by host polling
# (0.2 s over a growing transcript) plus the peer's own await, and CI runs at
# several times this box's load. Measured: see the late-joiner-stream echo.
BURST_N=3000

sshd_build
# sshd finds the key at the root of its own volume with no -k: the default
# this suite proves.
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
# A burst of console output, as fast as the Shell can echo.
cat > "$TEST_STAGING/burst.nsh" << NSH
@echo -off
for %a run (1 $BURST_N)
  echo %T%-ls%a
endfor
NSH
# A page of output, longer than any console, to put the prompt at the bottom.
for ((i = 1; i <= 80; i++)); do printf 'fill-%d\r\n' "$i"; done > "$TEST_STAGING/fill.txt"
cat > "$TEST_STAGING/loop.nsh" << 'NSH'
@echo -off
for %a run (1 300)
  echo %T%-it%a
  stall 100000
endfor
echo %T%-loopdone
NSH

echo "=== sshd: the live UEFI Shell over SSH ($TEST_ARCH) ==="
sshd_boot ""

# Type SQ_N command lines on the serial console in ONE write ($1 tags them),
# then check every one ran exactly once. Each output line is 70 characters,
# whole on one console row, so it is matched exactly.
serial_paste() {
    local tag=$1 label=$2 i lines='' n
    for ((i = 1; i <= SQ_N; i++)); do
        lines+="echo %T%-$tag$i-$SQ_RAND-$(printf '%050d' "$i")\\r"
    done
    echo "  $label: ${#lines} bytes typed in one write"
    send "$lines"
    wait_guest "p4f-$tag$SQ_N-$SQ_RAND-$(printf '%050d' "$SQ_N")" 1 40 || true
    sleep 1
    for ((i = 1; i <= SQ_N; i++)); do
        n=$(guest_lines "p4f-$tag$i-$SQ_RAND-$(printf '%050d' "$i")")
        check "$label: pasted line $i of $SQ_N ran exactly once ($n)" test "$n" = 1
    done
}

# A Ctrl-C behind AHEAD_N typed-ahead keys into loop.nsh breaks it: the
# command typed after it runs within AHEAD_S seconds (an unbroken loop runs
# for 30 s) and the loop never finishes. $1 tags it.
AHEAD_S=10
ctrl_c_ahead() {
    local tag=$1 label=$2 it0 done0 t0 t1 dt n
    it0=$(guest_lines "p4f-it3")
    done0=$(guest_lines "p4f-loopdone")
    send 'loop.nsh\r'
    wait_guest "p4f-it3" $((it0 + 1)) 30 || true
    send "$(printf 'y%.0s' $(seq 1 "$AHEAD_N"))"
    send '\x03'
    t0=$(date +%s.%N)
    send '\x1b'
    send "echo %T%-$tag$SQ_RAND\\r"
    wait_guest "p4f-$tag$SQ_RAND" 1 45 || true
    t1=$(date +%s.%N)
    dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
    check "$label: a Ctrl-C behind $AHEAD_N typed-ahead keys brings the prompt back within ${AHEAD_S}s (${dt}s)" \
        awk -v d="$dt" -v m="$AHEAD_S" 'BEGIN { exit !(d <= m) }'
    n=$(guest_lines "p4f-$tag$SQ_RAND")
    check "$label: the command typed after it runs exactly once ($n)" test "$n" = 1
    n=$(guest_lines "p4f-loopdone")
    check "$label: the script never finished ($((n - done0)))" test "$n" = "$done0"
}

# --- status-before ----------------------------------------------------------
send 'sshd status\r'
check "status-before: \`sshd status\` prints 'sshd: stopped'" \
    wait_guest "sshd: stopped" 1 20

# --- serial-paste-plain -----------------------------------------------------
serial_paste sqp serial-paste-plain

# --- ctrl-c-ahead-plain -----------------------------------------------------
ctrl_c_ahead cap ctrl-c-ahead-plain

# --- start ------------------------------------------------------------------
send "sshd $SSHD_LOG_HERE -P $PW -x $HOST_AS_SEEN\\r"
check "start: the launcher prints its serving line" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 1 60
check "start: the driver logs its serving line with the console size" \
    sshd_read_geometry

# --- serial-paste -----------------------------------------------------------
serial_paste sqs serial-paste

# --- ctrl-c-ahead -----------------------------------------------------------
ctrl_c_ahead cas ctrl-c-ahead

RAND=$(rand_tag)

# --- shell-command ----------------------------------------------------------
ssh_open A
ssh_wait_repaint A 40 || true
sleep 1
n=$(ssh_banner_count A)
check "shell-command: ssh(1) prints the console geometry banner once ($n)" \
    test "$n" = 1
first=$(head -c 4 "$(ssh_out A)" | od -An -c | tr -s ' ')
check "shell-command: the session's first bytes are the repaint's clear ($first)" \
    test "$(head -c 4 "$(ssh_out A)")" = $'\e[2J'
ssh_type A "echo %T%-$RAND\\r"
check "shell-command: the command's output line arrives in the session" \
    wait_session "$(ssh_out A)" "p4f-$RAND" 1 30
sleep 1
n=$(session_count "$(ssh_out A)" "p4f-$RAND")
check "shell-command: the output line arrives in the session exactly once ($n)" \
    test "$n" = 1
n=$(guest_lines "p4f-$RAND")
check "shell-command: the serial console shows the same line exactly once ($n)" \
    test "$n" = 1

# --- stderr -----------------------------------------------------------------
# A tool's error goes to StdErr (axl_printerr). The take-over joins the StdErr
# fan-out as it joins ConOut's, so the session SHOWS the line as a local
# screen in ErrOut does -- once -- and the serial console still shows it once.
# Judged on the RENDERED screen of a terminal the console's size (tmux), after
# the next prompt is drawn: the bytes can carry a line the screen no longer
# shows. The Shell places its prompt where IT last left the cursor, which the
# StdErr write did not move -- so mid-screen the prompt is drawn over the
# error's first columns, on a real screen in ErrOut as in the session (the
# GOP and the session agree, measured both ways). At the bottom, where the
# error scrolled the screen, the prompt lands on a fresh row and the line
# stays whole: that is the case asserted, after a page of output.
ERRLINE='sshd: -x bogus is not an IPv4 address or CIDR range'
tmux_ssh_open_sized T "$COLS" "$ROWS"
tmux_wait_screen T 'FS0:' 40 || true
tmux_text T 'cls'
tmux_keys T Enter
sleep 1
tmux_text T 'type fill.txt'
tmux_keys T Enter
tmux_wait_screen T "fill-80" 20 || true
tmux_text T 'sshd -T -x bogus'
tmux_keys T Enter
wait_session "$(ssh_out A)" "$ERRLINE" 1 20 || true
sleep 2
T_SCREEN=$(tmux_screen T 2>/dev/null | sed -E 's/[[:space:]]+$//' || true)
n=$(grep -cxF -- "$ERRLINE" <<< "$T_SCREEN" || true)
check "stderr: the session's screen shows the tool's error line exactly once (${n:-0})" \
    test "${n:-0}" = 1
last=$(grep -v '^$' <<< "$T_SCREEN" | tail -1 || true)
check "stderr: and the next prompt is drawn after it ('$last')" test "$last" = 'FS0:\>'
tmux_close T
# The same line in A's byte stream: the control for the sshd-log checks below.
n=$(session_count "$(ssh_out A)" "$ERRLINE")
check "stderr: a tool's error line reaches session A's stream exactly once ($n)" test "$n" = 1
n=$(guest_lines "$ERRLINE")
check "stderr: the serial console shows it exactly once ($n)" test "$n" = 1
# sshd's OWN log is the operator's, never the user's terminal (sshd logs to
# syslog): A's login was logged, and the session that just showed a tool's
# StdErr line -- the control above, the same capture -- shows no line of the
# driver's log (its domains are ssh and sshd). Another program's log line is
# a different matter: the launcher just run printed its leak verdict
# ("[INFO]  mem: ..." on a DEBUG build) to StdErr, and that is the session's
# to see, as a screen in ErrOut would.
n=$(session_lines "$(ssh_out A)" | grep -acF 'ssh: Accepted' || true)
check "sshd-log: the login's 'ssh: Accepted' log line is not in the session (${n:-0})" \
    test "${n:-0}" = 0
n=$(session_lines "$(ssh_out A)" | grep -acE '\[(ERROR|WARN|INFO|DEBUG|TRACE)\] +sshd?(:[a-z_0-9]+:[0-9]+)?: ' || true)
check "sshd-log: no line of the driver's log is in the session (${n:-0})" test "${n:-0}" = 0
session_lines "$(ssh_out A)" | grep -aE -e '\[[A-Z]+\] +sshd?[:]' | sed 's/^/    session: /' || true
n=$(guest_re "${LOG_PFX}ssh: Accepted publickey for anyone from 10\\.0\\.2\\.2 port [0-9]+\$")
check "sshd-log: the serial log has A's and T's 'ssh: Accepted publickey' lines ($n)" \
    test "$n" = 2
# The control: a Shell command's own error (its StdOut) is seen once either way.
ssh_type A "ls nosuch$RAND\\r"
LSERR="ls: File Not Found - 'FS0:\\'"
wait_session "$(ssh_out A)" "$LSERR" 1 20 || true
sleep 1
n=$(session_count "$(ssh_out A)" "$LSERR")
check "stderr: control -- a Shell error line reaches the session exactly once ($n)" test "$n" = 1
n=$(guest_lines "$LSERR")
check "stderr: control -- the serial console shows it exactly once ($n)" test "$n" = 1

# --- paste ------------------------------------------------------------------
# 200 bytes in one write: `echo %T%-<rand>-` (18), a 181-digit payload, CR. Any
# key lost past the ring's 63 changes the payload or loses the Enter.
PAYLOAD=$(printf '%s' 0123456789{0..17} | head -c 181)
LINE=$(printf 'echo %%T%%-%s-%s' "$RAND" "$PAYLOAD")
if (( ${#LINE} + 1 != 200 )); then
    echo "FAIL: the paste fixture is $(( ${#LINE} + 1 )) bytes, not 200"
    exit 1
fi
ssh_type A "$LINE\\r"
for _i in $(seq 1 150); do
    (( $(session_joined_count "$(ssh_out A)" "p4f-$RAND-$PAYLOAD") >= 1 )) && break
    sleep 0.2
done
sleep 1
n=$(session_joined_count "$(ssh_out A)" "p4f-$RAND-$PAYLOAD")
check "paste: the pasted line's whole output arrives in the session exactly once ($n)" \
    test "$n" = 1

# --- paste-lines ------------------------------------------------------------
# 70 command lines, ~4.6 KB, one write. Each line's output fits a console line.
TAIL40=0123456789012345678901234567890123456789
PASTE=""
for i in $(seq -w 1 70); do
    PASTE+=$(printf 'echo %%T%%-k%s-%s-%s' "$i" "$RAND" "$TAIL40")
    PASTE+=$'\r'
done
printf '%s' "$PASTE" >&"${SSH_FD[A]}"
echo "  paste-lines: ${#PASTE} bytes in one write"
wait_session "$(ssh_out A)" "p4f-k70-$RAND-$TAIL40" 1 90 || true
sleep 1
once=0
for i in $(seq -w 1 70); do
    if [[ $(session_count "$(ssh_out A)" "p4f-k$i-$RAND-$TAIL40") == 1 ]]; then
        once=$((once + 1))
    fi
done
check "paste-lines: all 70 pasted lines ran, each output exactly once ($once)" \
    test "$once" = 70

# --- idle -------------------------------------------------------------------
# A connected session on a console nobody writes to receives NOTHING: the
# 10 ms tick flushes an empty encoder and owes no repaint. A session stuck
# BEHIND, or a repaint that never completes, would resend the screen here
# every tick. This stretch is also where the CPU of an idle take-over is
# measured.
sleep 1
size0=$(stat -c %s "$(ssh_out A)")
echo "IDLE-BEGIN $(date +%s.%N)" > "$TEST_TMPDIR/idle.mark"
sleep "$IDLE_S"
echo "IDLE-END $(date +%s.%N)" >> "$TEST_TMPDIR/idle.mark"
size1=$(stat -c %s "$(ssh_out A)")
check "idle: ${IDLE_S}s on a quiet console sends the session nothing ($size0 -> $size1 bytes)" \
    test "$size0" = "$size1"

# --- two-sessions -----------------------------------------------------------
ssh_open B
ssh_wait_repaint B 40 || true
sleep 1
ssh_type A "echo %T%-a$RAND\\r"
wait_session "$(ssh_out B)" "p4f-a$RAND" 1 20 || true
sleep 1
n=$(session_count "$(ssh_out B)" "p4f-a$RAND")
check "two-sessions: what A runs shows in B exactly once ($n)" test "$n" = 1
ssh_type B "echo %T%-b$RAND\\r"
wait_session "$(ssh_out A)" "p4f-b$RAND" 1 20 || true
sleep 1
n=$(session_count "$(ssh_out A)" "p4f-b$RAND")
check "two-sessions: what B types runs and shows in A exactly once ($n)" test "$n" = 1
n=$(guest_lines "p4f-b$RAND")
check "two-sessions: and on the serial console exactly once ($n)" test "$n" = 1
ssh_close B

# --- late-joiner ------------------------------------------------------------
ssh_type A "echo %T%-lj$RAND\\r"
wait_session "$(ssh_out A)" "p4f-lj$RAND" 1 20 || true
sleep 0.5
ssh_open C
ssh_wait_repaint C 40 || true
sleep 2
n=$(ssh_banner_count C)
check "late-joiner: ssh(1) prints the console geometry banner once ($n)" test "$n" = 1
check "late-joiner: the joiner's first bytes are the repaint's clear" \
    test "$(head -c 4 "$(ssh_out C)")" = $'\e[2J'
n=$(session_count "$(ssh_out C)" "p4f-lj$RAND")
check "late-joiner: the marker already on screen is in the repaint exactly once ($n)" \
    test "$n" = 1
ssh_close C

# --- late-joiner-stream -----------------------------------------------------
# D joins while the console is busy, so at nearly every tick some output is
# buffered and not yet sent: the join must flush it to everyone BEFORE taking
# D's snapshot, or D gets those lines twice (in the snapshot, then live).
# D is ssh-peer.py, logged in and waiting before the burst starts, so its
# shell request -- the join -- lands mid-burst on demand; ssh(1)'s key
# exchange would take longer than the whole burst on a fast host.
peer_start D "$PW" "await:$TEST_TMPDIR/d.go,send:90:$(peer_open 1048576),expect:91,send:98:$(peer_req shell 1),expect:99,expectdata:100000000"
wait_peer_line D "peer: authenticated" 60 || true
burst_t0=$(date +%s.%N)
ssh_type A 'burst.nsh\r'
# Past a screenful, so the burst's first line has scrolled off before D
# joins: what D sees is then a window of the burst, never all of it.
wait_session "$(ssh_out A)" "p4f-ls$((ROWS + 20))" 1 30 || true
burst_go=$(date +%s.%N)
touch "$TEST_TMPDIR/d.go"
wait_session "$(ssh_out A)" "p4f-ls$BURST_N" 1 90 || true
burst_t1=$(date +%s.%N)
sleep 1
peer_stop D
peer_save D "$TEST_TMPDIR/D.out"
distinct=$(session_lines "$TEST_TMPDIR/D.out" | grep -axE 'p4f-ls[0-9]+' | sort -u | wc -l)
dups=$(session_lines "$TEST_TMPDIR/D.out" | grep -axE 'p4f-ls[0-9]+' | sort | uniq -d | wc -l)
# Reads to the end (no early exit): under pipefail a SIGPIPE upstream fails it.
first=$(session_lines "$TEST_TMPDIR/D.out" | awk '!f && /^p4f-ls[0-9]+$/ { print; f = 1 }')
echo "  late-joiner-stream: D saw $distinct distinct burst lines, from ${first#p4f-ls}; told to join at line $((ROWS + 20)) after $(bc <<< "$burst_go - $burst_t0")s, burst over after $(bc <<< "$burst_t1 - $burst_t0")s; load $(cut -d' ' -f1 /proc/loadavg)"
check "late-joiner-stream: D's first bytes are the repaint's clear" \
    test "$(head -c 4 "$TEST_TMPDIR/D.out")" = $'\e[2J'
check "late-joiner-stream: D joined mid-burst (it saw more than a screen, less than all: $distinct of $BURST_N)" \
    test "$distinct" -gt "$ROWS" -a "$distinct" -lt "$BURST_N"
check "late-joiner-stream: no burst line reached D twice ($dups duplicated)" test "$dups" = 0
n=$(session_count "$TEST_TMPDIR/D.out" "p4f-ls$BURST_N")
check "late-joiner-stream: the burst's last line reached D exactly once ($n)" test "$n" = 1

# --- ctrl-c -----------------------------------------------------------------
# One iteration is 0.1 s of stall plus an echo, so 300 take 30 s or more: a
# prompt within CTRL_C_S seconds of the Ctrl-C means the break landed.
ssh_type A 'loop.nsh\r'
check "ctrl-c: the looping script is running" \
    wait_session "$(ssh_out A)" "p4f-it3" 1 30
ssh_type A '\x03'
t0=$(date +%s.%N)
ssh_type A "echo %T%-ac$RAND\\r"
wait_session "$(ssh_out A)" "p4f-ac$RAND" 1 40 || true
t1=$(date +%s.%N)
dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
check "ctrl-c: the prompt is back and runs a command within ${CTRL_C_S}s of Ctrl-C (${dt}s)" \
    awk -v d="$dt" -v m="$CTRL_C_S" 'BEGIN { exit !(d <= m) }'
n=$(session_count "$(ssh_out A)" "p4f-ac$RAND")
check "ctrl-c: the command typed after it runs exactly once ($n)" test "$n" = 1
n=$(session_count "$(ssh_out A)" "p4f-loopdone")
check "ctrl-c: the script never finished ($n)" test "$n" = 0
ssh_type A 'sshd status\r'
check "ctrl-c: sshd is still running" wait_guest "sshd: running on $GUEST_IP" 1 20

# --- ctrl-c-stall -----------------------------------------------------------
ssh_type A "stall $STALL_US\\r"
t0=$(date +%s.%N)
sleep 0.5
ssh_type A '\x03'
ssh_type A "echo %T%-ns$RAND\\r"
wait_session "$(ssh_out A)" "p4f-ns$RAND" 1 40 || true
t1=$(date +%s.%N)
dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
check "ctrl-c-stall: a Ctrl-C does not cut a stall short (next command after ${dt}s >= ${STALL_MIN_S}s)" \
    awk -v d="$dt" -v m="$STALL_MIN_S" 'BEGIN { exit !(d >= m) }'
n=$(session_count "$(ssh_out A)" "p4f-ns$RAND")
check "ctrl-c-stall: the command typed behind it runs exactly once ($n)" test "$n" = 1

# --- ctrl-c-paste -----------------------------------------------------------
# The script never reads the console, so the paste fills the 63-key ring and
# the rest waits in the session's queue. No Enter in the paste: after the
# break the Shell reads the type-ahead as the start of its next line, and
# the Enter sent last completes it -- so the paste's output line proves no
# key of it was lost or reordered, and Ctrl-C itself added none.
PB=$(printf '%s' 0123456789{0..9} | head -c 100)
ssh_type A 'loop.nsh\r'
wait_session "$(ssh_out A)" "p4f-it3" 2 30 || true
ssh_type A "echo %T%-pb$RAND-$PB"
sleep 1
ssh_type A '\x03'
t0=$(date +%s.%N)
ssh_type A '\r'
for _i in $(seq 1 200); do
    (( $(session_joined_count "$(ssh_out A)" "p4f-pb$RAND-$PB") >= 1 )) && break
    sleep 0.2
done
t1=$(date +%s.%N)
dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
check "ctrl-c-paste: Ctrl-C behind a ${#PB}-digit paste breaks the script (the paste ran ${dt}s after it, <= ${CTRL_C_S}s)" \
    awk -v d="$dt" -v m="$CTRL_C_S" 'BEGIN { exit !(d <= m) }'
sleep 1
n=$(session_joined_count "$(ssh_out A)" "p4f-pb$RAND-$PB")
check "ctrl-c-paste: the paste then runs whole, exactly once ($n)" test "$n" = 1
n=$(session_count "$(ssh_out A)" "p4f-loopdone")
check "ctrl-c-paste: the script never finished ($n)" test "$n" = 0

# --- ctrl-c-serial ----------------------------------------------------------
# Session A stays open but idle: everything here is typed on the serial
# console, as an operator at the machine would. Earlier loops painted their
# lines on the serial console too, so the start is counted from a baseline.
it0=$(guest_lines "p4f-it3")
send 'loop.nsh\r'
check "ctrl-c-serial: the looping script is running" \
    wait_guest "p4f-it3" $((it0 + 1)) 30
send '\x03'
t0=$(date +%s.%N)
send "echo %T%-sc$RAND\\r"
wait_guest "p4f-sc$RAND" 1 40 || true
t1=$(date +%s.%N)
dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
check "ctrl-c-serial: a local Ctrl-C brings the prompt back within ${CTRL_C_S}s (${dt}s)" \
    awk -v d="$dt" -v m="$CTRL_C_S" 'BEGIN { exit !(d <= m) }'
n=$(guest_lines "p4f-sc$RAND")
check "ctrl-c-serial: the command typed after it runs exactly once ($n)" test "$n" = 1
n=$(guest_lines "p4f-loopdone")
check "ctrl-c-serial: the script never finished ($n)" test "$n" = 0

# --- split-input ------------------------------------------------------------
# Each `send:94` is its own SSH packet, and each packet is its own data
# callback in the server. Session A watches.
SP_LONG=$(printf '%s' 0123456789{0..14} | head -c 150)
OPEN=$(peer_open 1048576)
script="send:90:$OPEN,expect:91,send:98:$(peer_req shell 1),expect:99"
# a UTF-8 character (e-acute, C3 A9) cut after its lead byte, then an escape
# sequence (Left, ESC [ D) cut after its ESC: `Y` lands before the `X`
script+=",send:94:$(peer_data 'echo %T%-sp\xc3')"
script+=",send:94:$(peer_data "\\xa9$RAND"'X\x1b')"
script+=",send:94:$(peer_data '[DY\r')"
# LF alone, and CRLF, are each one Enter
script+=",send:94:$(peer_data "echo %T%-lf$RAND\\n")"
script+=",send:94:$(peer_data "echo %T%-cr$RAND\\r\\n")"
# a CRLF cut between its CR and its LF: still one Enter, and the next line
# starts clean
script+=",send:94:$(peer_data "echo %T%-cs$RAND\\r")"
script+=",send:94:$(peer_data "\\necho %T%-cz$RAND\\r")"
# a command longer than the key ring, in one packet
script+=",send:94:$(peer_data "echo %T%-pp$RAND-$SP_LONG\\n")"
script+=",hold:15"
peer_start split "$PW" "$script"
for _i in $(seq 1 150); do
    (( $(session_joined_count "$(ssh_out A)" "p4f-pp$RAND-$SP_LONG") >= 1 )) && break
    sleep 0.2
done
sleep 1
n=$(session_count "$(ssh_out A)" "p4f-spé${RAND}YX")
check "split-input: a split UTF-8 character and a split Left arrow arrive as their keys ($n)" \
    test "$n" = 1
n=$(session_count "$(ssh_out A)" "p4f-lf$RAND")
check "split-input: LF is Enter ($n)" test "$n" = 1
n=$(session_count "$(ssh_out A)" "p4f-cr$RAND")
check "split-input: CRLF is one Enter ($n)" test "$n" = 1
n=$(( $(session_count "$(ssh_out A)" "p4f-cs$RAND") * 10 + $(session_count "$(ssh_out A)" "p4f-cz$RAND") ))
check "split-input: a CRLF split across packets is one Enter, the next line clean ($n)" \
    test "$n" = 11
n=$(session_joined_count "$(ssh_out A)" "p4f-pp$RAND-$SP_LONG")
check "split-input: a ${#SP_LONG}-digit command in one packet runs whole ($n)" test "$n" = 1
peer_stop split

# --- exec-refused -----------------------------------------------------------
rc=0
timeout 60 ssh -p "$HOST_PORT" \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
    -o IdentityAgent=none -i "$TEST_TMPDIR/key" \
    anyone@127.0.0.1 "echo ex$RAND" > "$TEST_TMPDIR/exec.out" 2> "$TEST_TMPDIR/exec.err" || rc=$?
echo "  exec-refused: ssh exited $rc"
check "exec-refused: ssh(1) exits nonzero ($rc)" test "$rc" -ne 0
n=$(tr -d '\r' < "$TEST_TMPDIR/exec.err" | grep -acxF -- "exec request failed on channel 0" || true)
check "exec-refused: ssh(1) says 'exec request failed on channel 0' ($n)" test "${n:-0}" = 1
sleep 1
test_clean_log
n=$(grep -acF -- "ex$RAND" "$TEST_CLEAN_LOG" || true)
check "exec-refused: nothing reached the console (${n:-0} serial lines name the command)" \
    test "${n:-0}" = 0

ssh_close A

# --- status-running ---------------------------------------------------------
running0=$(guest_lines "sshd: running on $GUEST_IP")
send 'sshd status\r'
check "status-running: \`sshd status\` prints 'sshd: running on $GUEST_IP'" \
    wait_guest "sshd: running on $GUEST_IP" $((running0 + 1)) 20

# --- stop -------------------------------------------------------------------
send 'echo %T%-stop-begin\r'
wait_guest "p4f-stop-begin" 1 20 || true
send 'sshd stop\r'
check "stop: \`sshd stop\` prints 'sshd: stopped'" \
    wait_guest "sshd: stopped" 2 30
sleep 1
test_clean_log
# The driver's unload runs INSIDE `sshd stop`, before the launcher prints
# its result, so the verdicts between the marker and that line are the
# driver's alone. The launcher's own exit verdict follows the line.
window=$(awk '$0 == "p4f-stop-begin" { on = 1; next }
              on && $0 == "sshd: stopped" { exit }
              on { print }' "$TEST_CLEAN_LOG")
n_ok=$(grep -cE "${LOG_PFX}mem: no leaks detected\$" <<< "$window" || true)
n_leak=$(grep -cF "$TEST_LEAK_MARKER" <<< "$window" || true)
check "stop: the driver's own unload prints one clean leak verdict (clean=$n_ok leak=$n_leak)" \
    test "$n_ok" = 1 -a "$n_leak" = 0

# --- status-after -----------------------------------------------------------
send 'sshd status\r'
check "status-after: \`sshd status\` prints 'sshd: stopped' again" \
    wait_guest "sshd: stopped" 3 20

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
printf "sshd: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- session A (lines) ---"; session_lines "$(ssh_out A)" | tail -40
    echo "--- session A stderr ---"; tail -10 "$TEST_TMPDIR/A.err"
    echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 76 ]] && exit 0 || exit 1
