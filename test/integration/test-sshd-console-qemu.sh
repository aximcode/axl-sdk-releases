#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,tmux est=60 local-only=0 builds=sshd guest=none
# guest=none: it types into the guest Shell (test_send_stdin) and asserts
#   the serial console's bytes; a leased guest takes no typed serial input.
# test-sshd-console-qemu.sh -- while `sshd` serves, the machine's serial
# console behaves exactly as it did before: sshd writes nothing to a serial
# port the console draws on.
#
# Why it matters: the serial terminal driver draws by absolute cursor
# position from a cursor model of its own. A line it did not write -- sshd's
# log, and with -v one per packet -- scrolls the operator's terminal under
# it, and from then on the Shell's typed line and its output are painted on
# the wrong rows, over the log lines, and scroll away. On a server watched
# over serial-over-LAN that read as "the Shell's output stopped".
#
# The operator's screen is the oracle: the serial stream is fed to a real
# terminal (tmux, at the console's size) and its rows are matched exactly.
#
# Boots and scenarios, in run order:
#   one        one serial port, the console's.
#     default  `sshd -v` says the log is not written while serving, because
#              every port carries the console; with a session open and
#              typing (a packet each way per key), no log line reaches the
#              serial console, and a command typed on it reads on the
#              operator's screen exactly: the prompt and the command on one
#              row, its output on the next. The log is kept in memory:
#              `sshd log` prints the session's login line, as it would
#              have gone to the log. After `sshd stop` the console is still
#              the Shell's, and `sshd log` says sshd is not running.
#     none     `AxlLogPort none` in sshd.conf: `sshd -v` says so.
#     missing  `--log-port 7` names a port the machine does not have: sshd
#              refuses to start, saying so.
#     opt-in   `--log-port 0` puts the log on the console's own port when
#              asked: the session's login line reaches the serial console.
#   two        (x64) a second serial port with its terminal bound but not a
#              console, as a server's other COM port is (the platform
#              console drivers are disconnected from that terminal):
#              `sshd -v` sends the log there, and the login line is in that
#              port's output and not on the console.
#
# Usage: ./test/integration/test-sshd-console-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"

sshd_require_tools ssh ssh-keygen tmux
test_parse_args "$@"
test_setup

PW=con-pw
SKIPPED=0
NOLOG='the log is not written while serving: every serial port carries the console'
NOLOG_NONE='the log is not written while serving: AxlLogPort none'
NOSTART='sshd: could not start the server'
# A DEBUG line's message, after its "[DEBUG] sshd:func:line: ".
DBG_MSG_RE='\[DEBUG\] sshd:[a-z_0-9]+:[0-9]+: '
# Any log line, whatever its level and domain.
ANY_LOG_RE='\[(DEBUG|INFO|WARNING|ERROR)\] '
ACCEPTED_RE="${LOG_PFX}ssh: Accepted publickey for anyone from 10\\.0\\.2\\.2 port [0-9]+\$"

sshd_build
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
# For "none": a settings file that turns the log off, beside sshd.efi.
printf 'AxlLogPort none\r\n' > "$TEST_STAGING/none.conf"

# How many lines of FILE match the extended regex.
count_re() {
    local n
    n=$(grep -acE -- "$2" "$1" || true)
    echo "${n:-0}"
}

# wait_re_file FILE ERE N SECONDS -- wait for N lines of a raw serial capture
# (escapes and CRs removed) to match ERE.
wait_re_file() {
    local i n
    for ((i = 0; i < $4 * 5; i++)); do
        n=$(sed -E 's/\x1b\[[0-9;?=]*[a-zA-Z]//g' "$1" 2>/dev/null | tr -d '\r' \
            | grep -acE -- "$2" || true)
        (( ${n:-0} >= $3 )) && return 0
        sleep 0.2
    done
    return 1
}

# The console's size from the take-over's DEBUG line (written before the
# log moves, so it is on the console whatever the log port): COLS and ROWS.
console_size() {
    local line
    test_clean_log
    line=$(grep -aoE 'console device installed \([0-9]+x[0-9]+,' "$TEST_CLEAN_LOG" \
           | tail -1 || true)
    [[ -n "$line" ]] || return 1
    COLS=$(sed -E 's/.*\(([0-9]+)x([0-9]+),/\1/' <<< "$line")
    ROWS=$(sed -E 's/.*\(([0-9]+)x([0-9]+),/\2/' <<< "$line")
}

# render_until FIXED -- the operator's screen when FIXED was printed: the raw
# serial stream up to the end of FIXED's first occurrence, drawn by tmux at
# COLSxROWS, one row per line. Empty when FIXED is not in the stream.
render_until() {
    local off sock="$TEST_TMPDIR/tmux-render.sock" conf="$TEST_TMPDIR/tmux-render.conf"
    local raw="$TEST_TMPDIR/render.raw" i screen prev=""
    off=$(grep -abo -- "$1" "$TEST_LOG" | head -1 | cut -d: -f1 || true)
    [[ -n "$off" ]] || return 0
    head -c "$((off + ${#1}))" "$TEST_LOG" > "$raw"
    printf 'set -g status off\n' > "$conf"
    timeout 10 env -u TMUX tmux -S "$sock" -f "$conf" new-session -d \
        -x "$COLS" -y "$ROWS" "cat ${raw@Q}; sleep 60" || return 0
    # Settled when two captures 0.2 s apart agree and FIXED is drawn.
    for ((i = 0; i < 50; i++)); do
        screen=$(timeout 5 env -u TMUX tmux -S "$sock" capture-pane -p 2>/dev/null || true)
        if [[ "$screen" == "$prev" ]] && grep -qF -- "$1" <<< "$screen"; then
            break
        fi
        prev=$screen
        sleep 0.2
    done
    timeout 5 env -u TMUX tmux -S "$sock" kill-server 2>/dev/null || true
    rm -f "$sock"
    printf '%s\n' "$screen" | sed -E 's/[[:space:]]+$//'
}

# The sshd-domain DEBUG messages of the serial log, without their prefix.
sshd_debug_msgs() {
    test_clean_log
    { grep -aoE -- "${DBG_MSG_RE}.*\$" "$TEST_CLEAN_LOG" || true; } \
        | sed -E "s/^${DBG_MSG_RE}//"
}

# End a boot: power off, judge its leak verdicts, keep its log.
end_boot() {
    sshd_poweroff
    test_clean_log
    if test_check_leaks; then
        pass "$1: every leak verdict in the boot is clean"
    else
        fail "$1: a leak or a missing verdict"
        tail -40 "$TEST_CLEAN_LOG"
    fi
    n=$(heap_corruptions)
    check "$1: no heap corruption reported ($n)" test "$n" = 0
    cp -f "$TEST_LOG" "$TEST_TMPDIR/boot-$1.log"
}

echo "=== sshd and the serial console ($TEST_ARCH) ==="

# --- one: the only serial port is the console's -------------------------------
sshd_boot ""

send "sshd -v -P $PW\\r"
check "one/default: \`sshd -v\` serves" \
    wait_guest_re '^sshd: serving the UEFI Shell on 10\.0\.2\.15 port 22$' 1 60
check "one/default: it says the log is not written, and why, once" \
    test "$(sshd_debug_msgs | grep -cxF -- "$NOLOG" || true)" = 1
console_size || true
echo "  console: ${COLS:-?}x${ROWS:-?}"

R=$(rand_tag)
send "echo %T%-a-$R\\r"
wait_guest "p4f-a-$R" 1 20 || true
# A session that types: every key is a packet each way, the per-packet
# DEBUG lines that once flooded the console.
ssh_open s1
ssh_wait_repaint s1 30 || true
ssh_type s1 "echo %T%-s-$R\\r"
check "one/default: the session runs a command" \
    wait_session "$(ssh_out s1)" "p4f-s-$R" 1 30
send "echo %T%-b-$R\\r"
check "one/default: a command typed on the console runs" \
    wait_guest "p4f-b-$R" 1 20
test_clean_log
test_slice_log "p4f-a-$R" "p4f-b-$R" "$TEST_TMPDIR/ab.txt" || true
check "one/default: no log line reached the serial console while serving" \
    test "$(count_re "$TEST_TMPDIR/ab.txt" "$ANY_LOG_RE")" = 0
SCREEN=$(render_until "p4f-b-$R")
CMD_ROW=$(grep -nxF -- "FS0:\\> echo %T%-b-$R" <<< "$SCREEN" | head -1 | cut -d: -f1 || true)
OUT_ROW=$(grep -nxF -- "p4f-b-$R" <<< "$SCREEN" | head -1 | cut -d: -f1 || true)
if [[ -n "$CMD_ROW" && -n "$OUT_ROW" && $OUT_ROW -eq $((CMD_ROW + 1)) ]]; then
    pass "one/default: the operator's screen shows the prompt and command on one row, the output on the next"
else
    fail "one/default: the operator's screen shows the prompt and command on one row, the output on the next"
    printf '%s\n' "$SCREEN" | sed 's/^/    | /'
fi
send 'sshd log\r'
check "one/default: \`sshd log\` prints the session's login line from memory" \
    wait_guest_re '^\[INFO\] ssh: Accepted publickey for anyone from 10\.0\.2\.2 port [0-9]+$' 1 20
send 'sshd stop\r'
wait_guest "sshd: stopped" 1 30 || true
ssh_wait_exit s1 20 || true
send "echo %T%-c-$R\\r"
check "one/default: after \`sshd stop\` the console runs commands" \
    wait_guest "p4f-c-$R" 1 20
nr=$(guest_lines "sshd: not running")
send 'sshd log\r'
check "one/default: \`sshd log\` with no server says so" \
    wait_guest "sshd: not running" $((nr + 1)) 20

# none: the file turns the log off; -v says so.
send 'sshd -T -f none.conf\r'
check "one/none: \`sshd -T\` shows axllogport none" \
    wait_guest "axllogport none" 1 20
send "echo %T%-n-$R\\r"
wait_guest "p4f-n-$R" 1 20 || true
send "sshd -v -f none.conf -P $PW\\r"
wait_guest_re '^sshd: serving the UEFI Shell on 10\.0\.2\.15 port 22$' 2 60 || true
test_clean_log
test_slice_log "p4f-n-$R" "" "$TEST_TMPDIR/n.txt" || true
check "one/none: \`sshd -v\` says the file turned the log off" \
    test "$({ grep -aoE -- "${DBG_MSG_RE}.*\$" "$TEST_TMPDIR/n.txt" || true; } \
            | sed -E "s/^${DBG_MSG_RE}//" | grep -cxF -- "$NOLOG_NONE" || true)" = 1
send 'sshd stop\r'
wait_guest "sshd: stopped" 2 30 || true

# missing: a port the machine does not have is refused, by number.
send "sshd --log-port 7 -P $PW\\r"
check "one/missing: \`--log-port 7\` is refused, naming the port" \
    wait_guest_re "\\[ERROR\\] +sshd: AxlLogPort/--log-port 7: no such serial port \\(1 here\\)\$" 1 60
check "one/missing: the launcher says the server did not start" \
    wait_guest "$NOSTART" 1 10

# opt-in: the console's own port, asked for by number.
send "sshd --log-port 0 -P $PW\\r"
wait_guest_re '^sshd: serving the UEFI Shell on 10\.0\.2\.15 port 22$' 3 60 || true
n0=$(guest_re "$ACCEPTED_RE")
ssh_open s2
ssh_wait_repaint s2 30 || true
check "one/opt-in: \`--log-port 0\` puts the login line on the console's port" \
    wait_guest_re "$ACCEPTED_RE" $((n0 + 1)) 30
send 'sshd stop\r'
wait_guest "sshd: stopped" 3 30 || true
ssh_wait_exit s2 20 || true
end_boot one

# --- two: a second port the console does not use (x64) --------------------------
# aa64's virt machine's second UART is the firmware's DEBUG port (every
# AARCH64 guest has one: qemu_fw_debug_uart), not a COM port the server could
# take, so the case it models (a server's other COM port) only exists on x64.
if [[ "$TEST_ARCH" == X64 ]]; then
    COM2="$TEST_TMPDIR/com2.out"
    SSHD_QEMU_EXTRA=(-serial mon:stdio -serial "file:$COM2")
    sshd_boot ""
    # A server's other COM port: the terminal driver is bound on it, but it
    # is not a console -- not in ConOut or ErrOut. OVMF puts every serial
    # port on the console, so take the second one's terminal out of it the
    # way the firmware decides membership: disconnect the platform console
    # drivers (which put the console tags on) from the terminal, which stays
    # bound to its port. dh names the port (`BD: SerialIO
    # DevicePath(..rial(0x1)/Uart(...))`), devtree its terminal child, and
    # drivers the two ConPlatformDxe bindings.
    send "echo %T%-dh-$R\\r"
    send 'dh -p SerialIo\r'
    send "echo %T%-dh-end-$R\\r"
    wait_guest "p4f-dh-end-$R" 1 20 || true
    test_clean_log
    test_slice_log "p4f-dh-$R" "p4f-dh-end-$R" "$TEST_TMPDIR/dh.txt" || true
    H2=$(grep -aoE '^ *[0-9A-Fa-f]+: SerialIO .*rial\(0x1\)/Uart' "$TEST_TMPDIR/dh.txt" \
         | head -1 | sed -E 's/^ *([0-9A-Fa-f]+):.*/\1/' || true)
    [[ -n "$H2" ]] || { echo "FAIL: dh shows no second serial port"; exit 1; }
    send "echo %T%-dt-$R\\r"
    send "devtree $H2\\r"
    send 'drivers\r'
    send "echo %T%-dt-end-$R\\r"
    wait_guest "p4f-dt-end-$R" 1 20 || true
    test_clean_log
    test_slice_log "p4f-dt-$R" "p4f-dt-end-$R" "$TEST_TMPDIR/dt.txt" || true
    T2=$(grep -aoE "^ +Ctrl\\[[0-9A-Fa-f]+\\] .*Serial Console" "$TEST_TMPDIR/dt.txt" \
         | head -1 | sed -E 's/^ +Ctrl\[([0-9A-Fa-f]+)\].*/\1/' || true)
    CP=$(grep -aE 'ConPlatformDxe *$' "$TEST_TMPDIR/dt.txt" | awk '{print $1}' || true)
    [[ -n "$T2" && $(wc -w <<< "$CP") -eq 2 ]] \
        || { echo "FAIL: no terminal on port $H2 (${T2:-none}) or not two ConPlatform bindings (${CP:-none})"; exit 1; }
    echo "  second port: handle $H2, terminal $T2, console drivers" $CP
    for d in $CP; do
        send "disconnect $T2 $d\\r"
    done
    send "sshd -v -P $PW\\r"
    check "two/auto: \`sshd -v\` serves" \
        wait_guest_re '^sshd: serving the UEFI Shell on 10\.0\.2\.15 port 22$' 1 60
    check "two/auto: it says the log goes to the second port" \
        test "$(sshd_debug_msgs | grep -cxF -- 'the log goes to serial port 1 while serving' || true)" = 1
    ssh_open s3
    ssh_wait_repaint s3 30 || true
    check "two/auto: the login line is written to the second port" \
        wait_re_file "$COM2" "$ACCEPTED_RE" 1 30
    check "two/auto: and not to the console" \
        test "$(guest_re "$ACCEPTED_RE")" = 0
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 1 30 || true
    ssh_wait_exit s3 20 || true
    end_boot two
else
    echo "  SKIP: two -- one UART on this machine (6 checks)"
    SKIPPED=$((SKIPPED + 6))
fi

echo ""
WANT=$((22 - SKIPPED))
printf "sshd-console: %d passed, %d failed, %d skipped (%s)\n" "$PASS" "$FAIL" "$SKIPPED" "$TEST_ARCH"
[[ $FAIL -eq 0 && $PASS -eq $WANT ]] && exit 0 || exit 1
