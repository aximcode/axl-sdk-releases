#!/bin/bash
# test-meta: arch=x64 needs=ssh,ssh-keygen,sftp est=60 local-only=0 builds=sshd,sshd-faults,sftp-server guest=none
# guest=none: it types into the guest Shell (test_send_stdin), and a leased
#   guest takes no typed serial input.
# test-sshd-linger-qemu.sh -- sshd's AxlTcpLinger (--tcp-linger), which sets
# AxlTcpPacing.linger_us: after a reply's send retires, the connection stays
# polled, so a client with one request in flight is answered at once
# instead of at the firmware's next 10 ms timer and sshd's next tick.
#
# THE ASSERTION IS ON THE MECHANISM, COUNTED IN THE GUEST, not on wall time.
# What the linger promises is that a request arriving inside the window is
# received there and then; a faster transfer is a consequence of that, and
# host load moves the consequence without moving the mechanism (a 2x
# wall-clock margin collapsed once under load). The image is sshd-t.efi, the
# test build: its hook's "recv-stats" point (armed by fs0:\sshd-tcp-stats)
# makes AxlTcp log, as each connection that received closes, how many
# receives it completed, how many lingers began, and how many receives
# completed inside one. Wall times are printed, not judged.
#
# x64 on virtio-net (the NIC the knob is measured on; QEMU's e1000 drops
# frames under the extra polling). One boot, sshd started twice:
#
#   off      `sshd`: a 4 MiB `sftp -R 1` get; `sshd -T` says axltcplinger 0;
#            the connection's receives are counted (the control: the count
#            sees them) and none began a linger or completed inside one
#   on       `sshd --tcp-linger 9000`: the same get, byte for byte; at least
#            LINGER_SERVED_PCT of its GET_READS read requests were received
#            inside a linger
#   idle     with the linger on, an open ssh session doing nothing: the
#            guest's CPU over IDLE_SAMPLE_S, from /proc/<qemu>/stat at 5 Hz,
#            must stay below IDLE_CPU_MAX_PCT -- a linger ends, an idle
#            server never spins
#
# Usage: ./test/integration/test-sshd-linger-qemu.sh [--arch X64]
set -uo pipefail

# Before common-test.sh builds the QEMU command.
export TEST_NIC_MODEL="virtio-net-pci,romfile="
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"

# The linger the "on" run sets.
LINGER_US=9000
GET_BYTES=$((4 * 1048576))
# sftp's default read is 32 KiB, and -R 1 keeps one in flight: the get is
# this many read requests, each sent when the previous reply has arrived.
GET_READS=$((GET_BYTES / 32768))
# The share of them that must be received inside a linger. Measured (x64
# DEBUG, 12-cpu box): 145 receives inside a linger on a quiet box and at
# load 7 (12 busy loops); 139 and 71 at load 16-30 (24 busy loops), where
# the 2x wall-clock bound this replaced would have failed one run (8859 ms
# off against 7558 ms on) -- the count falls only when the CLIENT, starved
# itself, sends its next request after the 9 ms window. With the linger's
# polling sabotaged away it is 0, and with no linger 0, so a quarter keeps
# that floor far from both.
LINGER_SERVED_PCT=25
IDLE_SAMPLE_S=4
# A halted guest reads 0-3% (measured, RELEASE and DEBUG); a spinning one
# reads 100%. Host load cannot raise it -- it is QEMU's own CPU time, and a
# starved QEMU gets less, not more: 2% at load 7, 16 and 28 alike.
IDLE_CPU_MAX_PCT=25

sshd_require_tools ssh ssh-keygen sftp
test_parse_args "$@"
test_setup

declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_NATIVE[$TEST_ARCH]}" \
        all sshd-faults sftp-server \
        > "$TEST_TMPDIR/make.log" 2>&1 \
   || [[ ! -f "$(test_build_dir)/sshd-t.efi" ]]; then
    echo "FAIL: sshd-t.efi or SftpServer.efi did not build"
    tail -20 "$TEST_TMPDIR/make.log"
    exit 1
fi
mkdir -p "$TEST_STAGING"
cp "$(test_build_dir)/sshd-t.efi" "$TEST_STAGING/sshd.efi"
test_add_efi "$(test_build_dir)/tools/SftpServer.efi"
ssh-keygen -q -t ed25519 -N '' -C 'axl-sshd-test' -f "$TEST_TMPDIR/key" </dev/null
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
printf 'Subsystem sftp SftpServer.efi\r\n' > "$TEST_STAGING/sshd.conf"
: > "$TEST_STAGING/sshd-tcp-stats"
mkdir -p "$TEST_STAGING/lg"
head -c "$GET_BYTES" /dev/urandom > "$TEST_STAGING/lg/f.bin"
DL="$TEST_TMPDIR/dl"
mkdir -p "$DL"

CLIENT_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
             -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes
             -o IdentityAgent=none -o LogLevel=ERROR -i "$TEST_TMPDIR/key")

# timed_get NAME -- one `sftp -R 1` get of lg/f.bin; sets GET_MS and GET_RC.
timed_get() {
    local t0 t1
    rm -f "$DL/$1.bin"
    printf 'get /fs0/lg/f.bin %s.bin\n' "$1" > "$TEST_TMPDIR/$1.batch"
    t0=$(date +%s%N)
    (cd "$DL" && timeout 300 sftp -R 1 -b "$TEST_TMPDIR/$1.batch" -P "$HOST_PORT" \
        "${CLIENT_OPTS[@]}" anyone@127.0.0.1) > "$TEST_TMPDIR/$1.out" 2>&1 < /dev/null
    GET_RC=$?
    t1=$(date +%s%N)
    GET_MS=$(( (t1 - t0) / 1000000 ))
}

# start_sshd ARGS N -- start sshd with ARGS; the Nth serving line.
start_sshd() {
    send "sshd $SSHD_LOG_HERE $1\\r"
    wait_guest_re "${LOG_PFX}sshd: serving the UEFI Shell on .*" "$2" 60
}

# The close line of a connection that received (AxlTcp, armed by the hook).
STATS_RE="${LOG_PFX}tcp: closed after [0-9]+ receive\\(s\\); [0-9]+ linger\\(s\\), [0-9]+ receive\\(s\\) inside one$"

# stats_mark -- how many close lines the guest has logged so far: taken before
# a get, so the get's own line is found among the ones after it, not by its
# position in the whole log (another connection closing -- a probe, a retry
# -- would shift a position).
stats_mark() {
    STATS_MARK=$(guest_re "$STATS_RE")
}

# tcp_stats -- of the close lines logged since stats_mark, the get's: the one
# with the most receives (a 4 MiB get takes hundreds; anything else that
# closed beside it, a handful). Sets RECVS, LINGERS and SERVED. Returns 1,
# leaving them empty, when no new line came or it could not be read -- which
# no check below reads as a count.
# The close lines since the mark, as "receives lingers served", most
# receives last.
stats_since_mark() {
    test_clean_log
    grep -aE -- "$STATS_RE" "$TEST_CLEAN_LOG" | tail -n "+$((STATS_MARK + 1))" \
        | sed -E 's/.*closed after ([0-9]+) receive\(s\); ([0-9]+) linger\(s\), ([0-9]+) receive\(s\) inside one$/\1 \2 \3/' \
        | sort -n -k1,1
}

# tcp_stats -- the get's close line. It waits (up to 30 s) for a line since
# the mark that could BE the get's -- at least GET_READS receives -- not
# merely the first new one: a short connection that closes first would
# otherwise be read instead. If none comes, the largest line there is is
# read, so the off control (RECVS >= GET_READS) fails with the real number.
tcp_stats() {
    local line i
    RECVS="" LINGERS="" SERVED=""
    for ((i = 0; i < 150; i++)); do
        line=$(stats_since_mark | tail -1)
        [[ -n "$line" && "${line%% *}" -ge "$GET_READS" ]] && break
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || break
        sleep 0.2
    done
    [[ -n "$line" ]] || return 1
    read -r RECVS LINGERS SERVED <<<"$line"
    [[ "$RECVS" =~ ^[0-9]+$ && "$LINGERS" =~ ^[0-9]+$ && "$SERVED" =~ ^[0-9]+$ ]]
}

# The guest's CPU, percent of one core, over $1 seconds at 5 Hz.
qemu_cpu_pct() {
    local secs=$1 pid=$TEST_QEMU_PID a b i
    a=$(awk '{print $14 + $15}' "/proc/$pid/stat")
    for ((i = 0; i < secs * 5; i++)); do
        sleep 0.2
    done
    b=$(awk '{print $14 + $15}' "/proc/$pid/stat")
    echo $(( (b - a) * 100 / ($(getconf CLK_TCK) * secs) ))
}

echo "=== sshd linger ($TEST_ARCH, virtio-net) ==="
echo "  host load before the boot: $(cut -d' ' -f1-3 /proc/loadavg)"
sshd_boot ""

# --- off --------------------------------------------------------------------
check "off: sshd serves" start_sshd "" 1
send 'sshd -T\r'
check "off: sshd -T says axltcplinger 0 (the default)" wait_guest "axltcplinger 0" 1 20
stats_mark
timed_get off
OFF_MS=$GET_MS
check "off: sftp -R 1 get exits 0 ($GET_RC, ${OFF_MS} ms)" test "$GET_RC" = 0
check "off: the file arrives byte for byte" cmp -s "$DL/off.bin" "$TEST_STAGING/lg/f.bin"
check "off: the guest logs the get connection's receive counts" tcp_stats
echo "  off: $RECVS receive(s), $LINGERS linger(s), $SERVED receive(s) inside one"
check "off: the count sees the get's receives (${RECVS:-none} >= $GET_READS)" \
    test "${RECVS:-0}" -ge "$GET_READS"
check "off: no linger began, no receive completed inside one (${LINGERS:-none}, ${SERVED:-none})" \
    test "${LINGERS:-x}" = 0 -a "${SERVED:-x}" = 0

# --- on ---------------------------------------------------------------------
send 'sshd stop\r'
check "on: sshd stops" wait_guest "sshd: stopped" 1 30
check "on: sshd --tcp-linger $LINGER_US serves" start_sshd "--tcp-linger $LINGER_US" 2
stats_mark
timed_get on
ON_MS=$GET_MS
check "on: sftp -R 1 get exits 0 ($GET_RC, ${ON_MS} ms)" test "$GET_RC" = 0
check "on: the file arrives byte for byte" cmp -s "$DL/on.bin" "$TEST_STAGING/lg/f.bin"
check "on: the guest logs the get connection's receive counts" tcp_stats
echo "  on: $RECVS receive(s), $LINGERS linger(s), $SERVED receive(s) inside one"
check "on: at least ${LINGER_SERVED_PCT}% of the get's $GET_READS reads were received inside a linger (${SERVED:-none})" \
    test "$(( ${SERVED:-0} * 100 ))" -ge "$(( GET_READS * LINGER_SERVED_PCT ))"
echo "  sftp -R 1 get of $GET_BYTES B: ${OFF_MS} ms off, ${ON_MS} ms with a ${LINGER_US} us linger (information, not judged)"

# --- idle -------------------------------------------------------------------
(sleep $((IDLE_SAMPLE_S + 4))) | timeout $((IDLE_SAMPLE_S + 15)) ssh -tt -p "$HOST_PORT" \
    "${CLIENT_OPTS[@]}" anyone@127.0.0.1 > "$TEST_TMPDIR/idle.out" 2>&1 &
IDLE_SSH=$!
sleep 3
check "idle: the idle session is open" kill -0 "$IDLE_SSH"
CPU=$(qemu_cpu_pct "$IDLE_SAMPLE_S")
check "idle: with the linger on, an idle session costs ${CPU}% CPU (< ${IDLE_CPU_MAX_PCT}%)" \
    test "$CPU" -lt "$IDLE_CPU_MAX_PCT"
wait "$IDLE_SSH" 2>/dev/null || true
echo "  host load after the runs: $(cut -d' ' -f1-3 /proc/loadavg)"

sshd_poweroff
test_clean_log
if test_check_leaks; then
    pass "boot: every leak verdict is clean"
else
    fail "boot: a leak or a missing verdict"
fi
n=$(heap_corruptions)
check "boot: no heap corruption reported ($n)" test "$n" = 0

echo ""
echo "sshd-linger ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 ))
