# shellcheck shell=bash
# podman-run.sh -- run throwaway containers that this test OWNS, and remove
# exactly those when the test ends, however it ends.
#
# Sourced by a test (bash). WHY: `podman run --rm` removes its container only
# if the podman CLIENT survives to see it exit, and a killed test kills that
# client (measured 2026-11-09, podman 5.8):
#
#   - SIGTERM to the test (run-one.sh's `timeout` sends it to the whole process
#     group) reaches the client, which forwards it to the container's PID 1.
#     A `sh -c` there is PID 1 and IGNORES it, so the client never exits -- and
#     bash does not run a trap while a FOREGROUND child is still running, so
#     the test's own cleanup never starts either.
#   - SIGKILL (CTest's backstop, 3x the test limit -- now only once the
#     SIGTERM it sends first has gone unanswered for its grace, design §10.15)
#     kills the client outright. The container stays "Up" with no podman
#     process attached, forever.
#
# Both left stale containers that had to be `podman rm -f`'d by hand. The fix
# has three parts, one per failure:
#
#   - every container gets a NAME carrying this test's PID,
#     axl-it-<pid>-<rand>-<n>, so cleanup can name exactly what this run
#     started and never another run's or the user's;
#   - the client runs in the BACKGROUND and the test `wait`s on it. `wait`,
#     unlike a foreground child, is interrupted by a trapped signal, so the
#     TERM/INT trap installed here runs at once: it kills the client and
#     `podman rm -f`s each owned name;
#   - `--timeout` (AXL_PODMAN_CAP seconds, default 600) is enforced by conmon,
#     not the client, so even a SIGKILLed test's container is stopped -- and
#     then removed by `--rm`'s exit command -- within the cap.
#
# AXL_PODMAN_EXTRA_ARGS (word-split) is appended to every owned run. It is how
# test-container-lib.sh runs the consumer test with `--network none` to see
# what a mirror outage looks like, and how a person can reproduce one.
#
# Use:
#   source "$SCRIPT_DIR/lib/podman-run.sh"
#   podman_owned_init                      # once, before any run
#   podman_owned_run "$log" -v ... IMAGE sh -c "$script"; rc=$?
#   trap 'podman_owned_cleanup; rm -rf "$WORK"' EXIT  # cleanup is idempotent
#
# SIGNALS. The caller's EXIT trap -- which must call podman_owned_cleanup --
# has to run on TERM and INT too. A test that sources common-test.sh already
# has handlers for that (_test_killed: it prints the KILLED line naming the
# runner's limit, then dies by the signal through axl_die_by_signal, which
# runs the EXIT trap with further signals masked); podman_owned_init leaves
# those alone, because replacing them is the one thing common-test.sh says
# defeats them. Otherwise it installs axl-common.sh's axl_exit_on_signal,
# which ends the same way.

_AXL_PODMAN_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if ! declare -F axl_exit_on_signal >/dev/null; then
    # shellcheck source=/dev/null
    source "$_AXL_PODMAN_LIB_DIR/../../../scripts/axl-common.sh"
fi

AXL_PODMAN_CAP=${AXL_PODMAN_CAP:-600}
_AXL_PODMAN_NAMES=()
_AXL_PODMAN_CLIENT=""
_AXL_PODMAN_LOG=""

podman_owned_init() {
    # $$ is unique among live processes, which is what lets an outside
    # observer (test-container-lib.sh) find this run's containers from the
    # PID alone; the random part keeps a recycled PID from matching a
    # leftover of some earlier run.
    AXL_PODMAN_TAG="axl-it-$$-$RANDOM$RANDOM"
    [[ -n "$(trap -p TERM)" ]] || axl_exit_on_signal TERM
    [[ -n "$(trap -p INT)" ]] || axl_exit_on_signal INT
}

# podman_owned_run <log> <podman-run args>... -- returns the container's exit
# status (or the client's, if it never started one). Output goes to <log>.
podman_owned_run() {
    local log=$1; shift
    local name="$AXL_PODMAN_TAG-$(( ${#_AXL_PODMAN_NAMES[@]} + 1 ))"
    _AXL_PODMAN_NAMES+=("$name")
    local extra=()
    # shellcheck disable=SC2206
    [[ -n "${AXL_PODMAN_EXTRA_ARGS:-}" ]] && extra=($AXL_PODMAN_EXTRA_ARGS)
    podman run --rm --name "$name" --timeout "$AXL_PODMAN_CAP" \
        "${extra[@]}" "$@" > "$log" 2>&1 &
    _AXL_PODMAN_CLIENT=$!
    _AXL_PODMAN_LOG=$log
    local rc=0
    wait "$_AXL_PODMAN_CLIENT" || rc=$?
    _AXL_PODMAN_CLIENT=""
    return "$rc"
}

# Kill an in-flight client first, so it cannot create a container AFTER the
# rm below has looked for it; then remove every name this run handed out.
# `--ignore` because most of them already went away through --rm.
#
# An interrupted run's log is printed first: the caller's own EXIT trap is
# about to delete it, and it is the only record of how far the container got
# (an AXL-PKG-FAIL, or the last thing a stalled install printed).
podman_owned_cleanup() {
    if [[ -n "$_AXL_PODMAN_CLIENT" ]]; then
        kill -KILL "$_AXL_PODMAN_CLIENT" 2>/dev/null
        wait "$_AXL_PODMAN_CLIENT" 2>/dev/null
        _AXL_PODMAN_CLIENT=""
        echo "podman_owned_cleanup: interrupted ${_AXL_PODMAN_NAMES[-1]}; last lines of its output:"
        tail -12 "$_AXL_PODMAN_LOG" 2>/dev/null | sed 's/^/    /'
    fi
    local n
    for n in "${_AXL_PODMAN_NAMES[@]}"; do
        podman rm -f -t 0 --ignore "$n" >/dev/null 2>&1
    done
    _AXL_PODMAN_NAMES=()
}
