# shellcheck shell=bash
# ssh-guest-lib.sh -- guest-log checks every AxlSsh suite that boots
# ssh-server-selftest makes. Sourced after common-test.sh -- and CALLED only
# once the suite has defined its own check(), which it uses -- by
# test-ssh-auth-qemu.sh,
# test-ssh-policy-qemu.sh, test-ssh-transport-qemu.sh and
# lib/ssh-hostile-lib.sh (which carries it into the hostile, hostile-hooks,
# admission, live-flood and send-teardown suites). Never run on its own.
#
# NOT A TEST. lib/discover.sh schedules test/integration/test-*.sh only.

# ssh(1)'s whole line for AxlSsh's cap DISCONNECT -- the password cap and the
# 20-request cap send sshd's one text for both (auth.c:343) with RFC 4253's
# code 14. Only the port varies; a CR is allowed at the end (`ssh -vvv`
# transcripts carry one). ONE copy, here: the auth, policy, hostile and
# disconnect suites all read it.
CAP_DISC_RE='^Received disconnect from 127\.0\.0\.1 port [0-9]+:14: Too many authentication failures'$'\r''?$'

# ssh_check_no_reentry LABEL
#
# conn_drop's re-entry tripwire (src/net/axl-ssh-server.c) logs at ERROR and
# REFUSES the nested drop, so a regression there does not crash -- this line
# is the only thing that can see it. LABEL says how much of the run the log
# covers ("teardown" once SERVER: DONE is in; a narrower scope otherwise).
#
# A log grep cannot read is a FAILURE, not a zero: grep -c prints "0" and
# exits 1 for "read it, found none", and exits 2 for "could not read it" --
# the same empty-looking answer to two opposite questions.
ssh_check_no_reentry() {
    local label="$1" n rc=0
    test_clean_log
    n=$(grep -acF 'conn_drop re-entered' "$TEST_CLEAN_LOG") || rc=$?
    if [[ "$rc" -gt 1 ]]; then
        check "$label: conn_drop was never re-entered (guest log unreadable)" false
        return 0
    fi
    check "$label: conn_drop was never re-entered (${n:-0})" test "${n:-0}" = 0
}
