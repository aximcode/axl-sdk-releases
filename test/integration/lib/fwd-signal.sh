# shellcheck shell=bash
# fwd-signal.sh -- run a command so that a signal to THIS shell reaches it at
# once, as a TERM.
#
# Sourced (bash) by lib/run-one.sh and lib/pool-or-boot.sh, the two scripts
# ctest runs a test THROUGH. WHY: ctest's TIMEOUT_SIGNAL_NAME (the backstop's
# SIGTERM, cmake/AxlIntegrationTests.cmake.in) is sent to the test's own
# process alone -- never its process group, never its children -- and then,
# TIMEOUT_SIGNAL_GRACE_PERIOD later, ctest KILLs whatever is left of that
# process's tree (measured on CMake 4.2.8 and 4.3.6). Neither script could
# act on it:
#
#   - with no trap, bash dies of the TERM on the spot. Its child -- the
#     `timeout` around the test, or the `axlpool run` holding a lease -- is
#     orphaned, outside the tree ctest later KILLs, and runs on: worse than
#     the plain KILL it replaced, which at least reached it;
#   - with a trap, bash runs it only once a FOREGROUND child has ended, and
#     the child does not end: the KILL comes first, and no cleanup runs.
#
# `wait` on a BACKGROUND child is different: a trapped signal interrupts it,
# so the handler here runs at once. It passes a TERM to the child, whose own
# handling does the cleanup -- `timeout` forwards it to its whole process
# group (the test, whose common-test.sh handlers end its guests), `axlpool
# run` to its test's group and then releases the lease -- and fwd_wait goes on
# waiting for the child to end. lib/podman-run.sh solved the same deferral for
# its podman client the same way.
#
# Always a TERM, whatever arrived: it is the one signal every handler below
# acts on, and an INT would not do -- bash starts a background child with
# SIGINT ignored.
#
# Use:
#   fwd_signal_init                  trap INT, TERM and HUP (once, early)
#   fwd_run CMD...                   run CMD in the background, wait for it;
#                                    its status (redirect the CALL to redirect
#                                    CMD). 143 without running it when a
#                                    signal already came.
#   fwd_start CMD... / fwd_wait      the two halves, for a caller that has
#                                    something to start beside CMD (FWD_PID)
#   FWD_SIGNAL                       the first signal received, or empty: a
#                                    caller checks it before starting anything
#                                    more (a retry, a booted rerun)
#   fwd_signal_status                returns 128 + FWD_SIGNAL's number: the
#                                    status a stopped script exits with

FWD_SIGNAL=""
FWD_PID=""
_FWD_N=0

_fwd_on() {
    [[ -n "$FWD_SIGNAL" ]] || FWD_SIGNAL="$1"
    _FWD_N=$(( _FWD_N + 1 ))
    [[ -n "$FWD_PID" ]] && kill -TERM "$FWD_PID" 2>/dev/null
    return 0
}

fwd_signal_init() {
    trap '_fwd_on TERM' TERM
    trap '_fwd_on INT' INT
    trap '_fwd_on HUP' HUP
}

fwd_start() {
    FWD_PID=""
    [[ -z "$FWD_SIGNAL" ]] || return 1
    "$@" &
    FWD_PID=$!
    # A signal between the fork and the line above found no FWD_PID to pass
    # the TERM to.
    [[ -z "$FWD_SIGNAL" ]] || kill -TERM "$FWD_PID" 2>/dev/null
    return 0
}

fwd_wait() {
    local n rc r2
    [[ -n "$FWD_PID" ]] || return 143
    # A `wait` a signal interrupted returns >128 BEFORE the child has ended
    # (bash runs the trap right after); the count the trap bumps tells that
    # apart from a child that itself exited >128, and the child is waited
    # for again. A signal landing just as the child ended may find it already
    # reaped: a second `wait` then knows no such PID (127), and the status in
    # hand stands.
    while :; do
        n=$_FWD_N
        wait "$FWD_PID"; rc=$?
        [[ "$n" == "$_FWD_N" ]] && break
        kill -0 "$FWD_PID" 2>/dev/null && continue
        wait "$FWD_PID" 2>/dev/null; r2=$?
        [[ $r2 -ne 127 ]] && rc=$r2
        break
    done
    FWD_PID=""
    return "$rc"
}

# fwd_signal_status -- the status a script a signal stopped exits with,
# 128 + its number (143 for a TERM), whatever its child returned.
fwd_signal_status() {
    return $(( 128 + $(kill -l "${FWD_SIGNAL:-TERM}") ))
}

fwd_run() {
    fwd_start "$@" || return 143
    fwd_wait
}
