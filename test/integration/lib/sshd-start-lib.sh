# shellcheck shell=bash
# sshd-start-lib.sh -- the setup and helpers of the sshd start suites: how
# `sshd` starts, what it says while it does, where it says it serves, and
# how its network bring-up ends when there is no network to bring up. Each
# boot gives the guest a different network; sshd brings the network up
# ITSELF in all but the first (no `ifconfig` in the startup).
#
# THREE TESTS, SIX BOOTS. This was test-sshd-start-qemu.sh, six boots run
# one after another in ~140 s; one test is the unit the runner's pool
# parallelises, so the boots were split into three parts that source this
# file and run their boots exactly as before:
#
#   test-sshd-start-qemu.sh          two, race
#   test-sshd-start-nolease-qemu.sh  nolease
#   test-sshd-start-nonet-qemu.sh    linkdown, alldown, nonic
#
# Every check is in exactly one part; each part ends with
# sshd_start_finish and its own expected count.
#
# Every boot ends with a clean leak verdict for every image that unloaded.
# The driver sweep itself (drivers\<arch> beside sshd.efi) is
# test-sshd-sweep-qemu.sh.

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/linkdown-lib.sh"

test_parse_args "$@"
test_setup
declare -A _NAT=([X64]=x64 [AARCH64]=aa64)
NAT="${_NAT[$TEST_ARCH]:-x64}"

PW=start-pw
STARTING='sshd: starting -- bringing up the network (up to 60 s)'
STARTING25='sshd: starting -- bringing up the network (up to 25 s)'
NOSTART='sshd: could not start the server'
# A DEBUG line's message, after its "[DEBUG] sshd:func:line: ".
DBG_MSG_RE='\[DEBUG\] sshd:[a-z_0-9]+:[0-9]+: '

sshd_build
ld_setup "$TEST_ARCH" "$TEST_TMPDIR"

now() { date +%s.%N; }
# Whole seconds from $1 to $2, rounded down.
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%d", b - a }'; }

# The sshd-domain DEBUG messages in the serial log, in order, between the
# line that is exactly $1 (a marker echoed before the command) and the first
# line after it that starts with $2 (the line that ends the start).
phases_between() {
    test_clean_log
    awk -v from="$1" -v stop="$2" '
        $0 == from { on = 1; next }
        on && index($0, stop) == 1 { exit }
        on { print }' "$TEST_CLEAN_LOG" \
        | { grep -aoE -- "${DBG_MSG_RE}.*\$" || true; } | sed -E "s/^${DBG_MSG_RE}//"
}

# check_phases LABEL MARKER STOP EXPECTED -- the phases, exactly. A DHCP
# line's budget is what was left when it began, so it reads "up to N s".
check_phases() {
    local got
    got=$(phases_between "$2" "$3" \
          | sed -E 's/^(DHCP on NIC [0-9a-f:]+ \(up to )[0-9]+( s\))$/\1N\2/')
    if [[ "$got" == "$4" ]]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$4") <(printf '%s\n' "$got") | sed 's/^/    /' || true
    fi
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

# sshd_start_finish PART WANT -- the summary line and the exit status: every
# check passed and exactly WANT ran (WANT already net of any SKIPPED).
sshd_start_finish() {
    echo ""
    printf "%s: %d passed, %d failed, %d skipped (%s)\n" "$1" "$PASS" "$FAIL" "${SKIPPED:-0}" "$TEST_ARCH"
    [[ $FAIL -eq 0 && $PASS -eq $2 ]] && exit 0 || exit 1
}
