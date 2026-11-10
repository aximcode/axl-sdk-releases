#!/bin/bash
# test-signal-cleanup-selftest.sh — a signal ENDS run-qemu.sh and
# profile-qemu.sh after their cleanup, and a kill of any of them -- or of a
# common-test.sh test booting in the foreground -- takes the guest with it.
#
# Both scripts trapped INT and TERM with their cleanup and nothing else, and a
# bash trap handler that does not exit RESUMES the script. Measured before the
# fix, on a real guest:
#
#   run-qemu.sh (foreground) killed mid-run removed its state dir, then went on
#   into `sed` over the serial log it had just deleted -- "No such file or
#   directory" twice -- and exited 1. Its guest ran on to its own --timeout:
#   `timeout` runs QEMU in a process group of its own, so neither a caller's
#   group signal nor the cleanup ever reached it.
#
#   profile-qemu.sh killed while waiting for the app to load killed its guest
#   and deleted its work dir in the cleanup, resumed, carried on to the next
#   step and reported "[profile] gdb-syms.py failed" -- over a symbol file its
#   own cleanup had deleted, a diagnosis of a kill it had done itself. Where
#   the kill lands decides which false diagnosis it prints, so the check below
#   is "no [profile] line after the kill", not one message.
#
# Both now end through axl_die_by_signal (scripts/axl-common.sh): the EXIT
# trap runs once and the script dies by the signal. test-runner-selftest.sh
# holds common-test.sh's handler to the same contract.
#
# Real guests, so this needs QEMU + OVMF; a dummy .efi keeps each guest idle
# at the Shell for as long as its --timeout. Excluded from discovery by name
# (*-selftest.sh), like the other harness self-tests, and run by
# scripts/run-selftests.sh beside the other self-tests: the guest checks are by path, which a parallel
# pool would not disturb, but a kill test inside the pool is exactly the
# nesting run-selftests.sh exists to avoid.
#
# Every path is generated at runtime with mktemp, and a guest is recognised
# by its /proc comm as well as by the path in its argv, so nothing here can
# match the harness itself. Each case takes a positive control first: the
# guest must be SEEN while it runs before its absence afterwards means
# anything.
#
# Usage: ./test/integration/test-signal-cleanup-selftest.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
PROFILE_QEMU="$PROJECT_DIR/scripts/profile-qemu.sh"

fail=0
BASE=""
OUTD="$(mktemp -d)"
DUMMY="$(mktemp --suffix=.efi)"

harness_cleanup() {
    if [[ -n "$BASE" ]]; then
        pkill -9 -f -- "$BASE" 2>/dev/null
        sleep 0.3
        rm -rf "$BASE"
    fi
    rm -rf "$OUTD" "$DUMMY"
}
trap harness_cleanup EXIT

say_fail() { echo "  FAIL: $*"; fail=$((fail + 1)); }

# Guests under the current base: a process whose argv names it AND whose comm
# is qemu-system*. The `timeout` watchdog also carries the path, and must not
# be counted as a guest.
guests() {
    local pid comm n=0
    for pid in $(pgrep -f -- "$BASE" 2>/dev/null); do
        comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
        [[ "$comm" == qemu-system* ]] && n=$((n + 1))
    done
    echo "$n"
}
wait_guest_up() {      # up to 30 s
    for _ in $(seq 1 300); do
        [[ "$(guests)" -ge 1 ]] && return 0
        sleep 0.1
    done
    return 1
}
wait_no_guest() {      # up to $1 s
    for _ in $(seq 1 $(( $1 * 10 ))); do
        [[ "$(guests)" -eq 0 ]] && return 0
        sleep 0.1
    done
    return 1
}
new_base() {
    [[ -n "$BASE" ]] && { pkill -9 -f -- "$BASE" 2>/dev/null; sleep 0.3; rm -rf "$BASE"; }
    BASE=$(mktemp -d -p /dev/shm axlsigprobe.XXXXXXXX)
}

# Seams that put a signal at an exact instant inside run-qemu.sh, where load
# alone put it only sometimes. Sourced through BASH_ENV, so the scripts under
# test carry no hook: a function shadows the builtin of the same name, and
# fires once (the mark), only in the function named and only when SEAM asks.
# A refactor that renames what they key on stops them firing -- which each
# case's control catches (the mark must exist), rather than reading green.
#
#   SEAM=repeat  the repeat of a group kill. coreutils `timeout` TERMs its
#                child and then the child's whole group, and the second TERM
#                can land while axl_die_by_signal's own $(kill -l ...) runs
#                -- killing that subshell. Sent from inside it, to the group
#                (only when run-qemu.sh leads its own group: setsid below).
#   SEAM=launch  a TERM to run-qemu.sh alone just after a guest's launch
#                (the `&`), from _rq_tm_watch, which runs straight after
#                every booted launch -- once that guest is up.
#   SEAM=guard   the same TERM from inside _rq_guard_guest, the last instant
#                before the guard is in place. Both seams land while
#                _rq_guard_begin's recording handlers hold the signal; a
#                signal after the guard (during the `wait`) is cases 1 and 2.
SEAMS="$OUTD/seams.sh"
cat > "$SEAMS" <<'SEAMS_EOF'
_seam_fire() {
    [[ "${SEAM:-}" == "$1" && " ${FUNCNAME[*]} " == *" $2 "* && ! -e "$SEAM_MARK" ]] || return 1
    : > "$SEAM_MARK"
}
kill() {
    if [[ "${1:-}" == -l ]] && _seam_fire repeat axl_die_by_signal; then
        local st
        read -r -a st < "/proc/$$/stat"
        # "sent" only when it goes: a run-qemu.sh not leading its own group
        # (a setsid that forked) gets no repeat, and its control fails.
        # Written BEFORE the kill, which can end this very subshell.
        [[ "${st[4]}" == "$$" ]] && { echo sent > "$SEAM_MARK"; builtin kill -TERM -- "-$$"; }
    fi
    builtin kill "$@"
}
# Held until the guest is up -- its firmware has written to the serial log:
# a TERM before QEMU has opened its files only has the cleanup delete them
# under it, and QEMU then fails to start by itself, which would pass for a
# fix. (A qemu-system process alone is not enough: measured, it was still
# opening its disk.)
_seam_guest_up() {
    [[ -s "$LOG" ]]
}
_seam_term_once_up() {
    local _seam_i
    for (( _seam_i = 0; _seam_i < 300; _seam_i++ )); do   # up to 30 s
        _seam_guest_up && { echo up > "$SEAM_MARK"; break; }
        sleep 0.1
    done
    builtin kill -TERM "$$"
}
printf() {
    _seam_fire launch _rq_tm_watch && _seam_term_once_up
    builtin printf "$@"
}
trap() {
    _seam_fire guard _rq_guard_guest && _seam_term_once_up
    builtin trap "$@"
}
SEAMS_EOF

echo "=== signal cleanup: run-qemu.sh, profile-qemu.sh, a foreground test ==="

# --- 1. run-qemu.sh in the foreground, killed as a test is killed ------------
f0=$fail
# An outer `timeout`, which signals its whole process group: run-qemu.sh and
# everything in its group, but not the guest's own `timeout`.
new_base
AXL_QEMU_TMPDIR="$BASE" timeout 10 "$RUN_QEMU" --timeout 60 "$DUMMY" \
    >"$OUTD/g.out" 2>"$OUTD/g.err" &
tpid=$!
if ! wait_guest_up; then
    say_fail "control: run-qemu.sh (group kill) never showed a guest, so nothing below means anything"
fi
wait "$tpid"; rc=$?
[[ "$rc" -eq 124 ]] || say_fail "the outer timeout did not fire (status $rc)"
if grep -q 'No such file or directory' "$OUTD/g.err"; then
    say_fail "run-qemu.sh resumed after its cleanup on a group kill:"
    sed 's/^/        /' "$OUTD/g.err" | head -4
fi
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a group kill left run-qemu.sh's state dir: $(ls "$BASE")"
wait_no_guest 10 || say_fail "the guest outlived a group kill of run-qemu.sh ($(guests) still running)"
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh killed with its group: no resume, state dir gone, guest gone"

# --- 1b. ... and the group kill's REPEAT lands inside the signal handler -----
# Case 1 as CI failed it (2026-11-10, in parallel with other self-tests under
# load): the outer timeout's second TERM, to the group, killed the $(kill -l)
# subshell axl_die_by_signal had just forked. The empty result made its
# arithmetic a syntax error, which aborted the handler and RESUMED run-qemu.sh
# -- which dropped the guard on its guest and exited through a cleanup that
# no longer stopped it. The guest ran on to its --timeout. Load only moved
# the repeat into that window some of the time (8 runs in 48 under parallel
# load); the seam puts it there every time.
f0=$fail
new_base
mark="$OUTD/seam-repeat"
BASH_ENV="$SEAMS" SEAM=repeat SEAM_MARK="$mark" AXL_QEMU_TMPDIR="$BASE" \
    setsid "$RUN_QEMU" --timeout 60 "$DUMMY" >"$OUTD/r.out" 2>"$OUTD/r.err" &
rpid=$!
if ! wait_guest_up; then
    say_fail "control: run-qemu.sh (repeated group kill) never showed a guest, so nothing below means anything"
fi
kill -TERM -- "-$rpid" 2>/dev/null
# Bounded: a script that resumes would otherwise wait on its guest's 60 s.
for _ in $(seq 1 300); do kill -0 "$rpid" 2>/dev/null || break; sleep 0.1; done
kill -0 "$rpid" 2>/dev/null && { say_fail "run-qemu.sh still running 30 s after a repeated group TERM"; kill -KILL "$rpid"; }
wait "$rpid"; rc=$?
grep -qx sent "$mark" 2>/dev/null \
    || say_fail "control: the seam never sent the repeat from inside axl_die_by_signal, so nothing below means anything"
[[ "$rc" -eq 143 ]] || say_fail "a repeated group TERM did not end run-qemu.sh by SIGTERM (status $rc, want 143)"
if grep -qE 'syntax error|No such file or directory' "$OUTD/r.err"; then
    say_fail "run-qemu.sh's signal handler was cut short and the script resumed:"
    sed 's/^/        /' "$OUTD/r.err" | head -4
fi
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a repeated group TERM left run-qemu.sh's state dir: $(ls "$BASE")"
wait_no_guest 10 || say_fail "the guest outlived a repeated group TERM of run-qemu.sh ($(guests) still running)"
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh's group TERMed again inside its handler: dies by it, state dir gone, guest gone"

# --- 1c/1d. run-qemu.sh sent a TERM just after its guest's launch -----------
# The guard that kills the guest on the way out was armed only AFTER the
# launch and the timing watcher's start, so a signal handled in between (a
# few ms; any instant a bash trap can run) left a guest no kill of the script
# reached -- run under its own `timeout`, so a group kill misses it too.
#
# seam_term_case SEAM WHAT [run-qemu.sh option...]: run-qemu.sh with the seam
# sending its TERM; it must die by it, its state dir and its guest gone.
seam_term_case() {
    local seam="$1" what="$2" mark rc
    shift 2
    f0=$fail
    new_base
    mark="$OUTD/seam-$seam-$RANDOM"
    BASH_ENV="$SEAMS" SEAM="$seam" SEAM_MARK="$mark" SHOT_WAIT=4 AXL_QEMU_TMPDIR="$BASE" \
        timeout 60 "$RUN_QEMU" --timeout 90 "$@" "$DUMMY" >"$OUTD/seam.out" 2>"$OUTD/seam.err" &
    wait "$!"; rc=$?
    [[ -e "$mark" ]] \
        || say_fail "control: the $seam seam never sent its TERM ($what), so nothing below means anything"
    grep -qx up "$mark" 2>/dev/null \
        || say_fail "control: the guest was not up when the $seam seam sent its TERM ($what), so nothing below means anything"
    [[ "$rc" -eq 143 ]] || say_fail "a TERM $what did not end run-qemu.sh by SIGTERM (status $rc, want 143)"
    [[ -z "$(ls -A "$BASE")" ]] || say_fail "a TERM $what left run-qemu.sh's state dir: $(ls "$BASE")"
    wait_no_guest 10 || say_fail "the guest outlived a TERM of run-qemu.sh $what ($(guests) still running)"
    [[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh sent TERM $what: dies by it, state dir gone, guest gone"
}
seam_term_case launch "just after its guest's launch"
# _rq_guard_begin's recording handlers hold a signal from the launch until
# the guard is in place; this one lands in that instant.
seam_term_case guard "while its guard went up"

# --- 2. run-qemu.sh in the foreground, sent a TERM of its own ----------------
# A hand `kill` of the script alone. Nothing but its own cleanup can reach the
# guest, and the script must die by the signal rather than exit after resuming.
f0=$fail
new_base
AXL_QEMU_TMPDIR="$BASE" "$RUN_QEMU" --timeout 60 "$DUMMY" \
    >"$OUTD/d.out" 2>"$OUTD/d.err" &
rpid=$!
if ! wait_guest_up; then
    say_fail "control: run-qemu.sh (direct TERM) never showed a guest, so nothing below means anything"
fi
kill -TERM "$rpid" 2>/dev/null
wait "$rpid"; rc=$?
[[ "$rc" -eq 143 ]] || say_fail "a direct TERM did not end run-qemu.sh by SIGTERM (status $rc, want 143)"
if grep -q 'No such file or directory' "$OUTD/d.err"; then
    say_fail "run-qemu.sh resumed after its cleanup on a direct TERM:"
    sed 's/^/        /' "$OUTD/d.err" | head -4
fi
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a direct TERM left run-qemu.sh's state dir: $(ls "$BASE")"
wait_no_guest 10 || say_fail "the guest outlived a direct TERM of run-qemu.sh ($(guests) still running)"
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh sent TERM: dies by it, no resume, state dir gone, guest gone"

# --- 2b. run-qemu.sh --background, sent a TERM BEFORE its handover -----------
# A background guest outlives run-qemu.sh by design -- once its caller has the
# QEMU_PID= line. Before that line the caller cannot know the guest exists, so
# a signal in that window (~2 s of hostfwd checking after the launch) must take
# the guest down. It did not: `timeout` runs QEMU in a process group of its
# own and the owner subshell defers its trap while timeout runs, so a TERM to
# a profile-qemu.sh still inside `$(run-qemu.sh --background ...)` left the
# guest running to its --timeout (found 2026-11-08, by an interrupted
# run-selftests.sh). The control: no QEMU_PID= line was printed, so the TERM
# really landed before the handover.
f0=$fail
new_base
AXL_QEMU_TMPDIR="$BASE" "$RUN_QEMU" --background --timeout 60 "$DUMMY" \
    >"$OUTD/b.out" 2>"$OUTD/b.err" &
rpid=$!
if ! wait_guest_up; then
    say_fail "control: run-qemu.sh --background never showed a guest, so nothing below means anything"
fi
kill -TERM "$rpid" 2>/dev/null
wait "$rpid"; rc=$?
if grep -q '^QEMU_PID=' "$OUTD/b.out"; then
    say_fail "control: the TERM landed AFTER the handover (QEMU_PID= printed) -- this proves nothing"
fi
[[ "$rc" -eq 143 ]] || say_fail "a TERM before the handover did not end run-qemu.sh --background by SIGTERM (status $rc, want 143)"
wait_no_guest 10 || say_fail "the guest outlived a TERM of run-qemu.sh --background before its handover ($(guests) still running)"
# The owner removes the state dir once its guest is gone: asynchronous, so
# bounded-polled rather than read once.
for _ in $(seq 1 50); do [[ -z "$(ls -A "$BASE")" ]] && break; sleep 0.1; done
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a TERM before the handover left run-qemu.sh --background's state dir: $(ls "$BASE")"
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh --background sent TERM before its handover: dies by it, guest gone, state dir gone"

# --- 2c. run-qemu.sh --background, SIGKILLed before its handover ------------
# The launcher dying in a way that runs NO trap. A guard in run-qemu.sh's own
# EXIT trap cannot cover this, and was measured not to cover a TERM either:
# under a group TERM the launcher sometimes died part-way through that trap
# and the guest ran on (3 of 10 at one timing). So the OWNER subshell -- which
# outlives the launcher by design -- watches the launcher until the handover,
# and takes the guest down itself if the launcher is gone first.
f0=$fail
new_base
AXL_QEMU_TMPDIR="$BASE" "$RUN_QEMU" --background --timeout 60 "$DUMMY" \
    >"$OUTD/k.out" 2>"$OUTD/k.err" &
rpid=$!
if ! wait_guest_up; then
    say_fail "control: run-qemu.sh --background (SIGKILL) never showed a guest, so nothing below means anything"
fi
kill -KILL "$rpid" 2>/dev/null
wait "$rpid" 2>/dev/null
if grep -q '^QEMU_PID=' "$OUTD/k.out"; then
    say_fail "control: the SIGKILL landed AFTER the handover (QEMU_PID= printed) -- this proves nothing"
fi
wait_no_guest 10 || say_fail "the guest outlived a SIGKILL of run-qemu.sh --background before its handover ($(guests) still running)"
for _ in $(seq 1 50); do [[ -z "$(ls -A "$BASE")" ]] && break; sleep 0.1; done
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a SIGKILL before the handover left run-qemu.sh --background's state dir: $(ls "$BASE")"
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh --background SIGKILLed before its handover: the owner takes the guest down, state dir gone"

# --- 2d. the OWNER sent a TERM before the handover, the launcher alive -------
# A group TERM reaches the owner as well as the launcher; its own handler must
# end the guest -- not leave it to the launcher-gone poll, which a group TERM
# also trips and so hid a missing handler (nothing failed with it deleted).
# Here only the owner is signalled: the launcher lives and hands over, so the
# owner's handler is the only thing that can end the guest. The owner is the
# parent of the guest's `timeout` (the process whose argv names the base and
# whose comm is timeout).
f0=$fail
new_base
AXL_QEMU_TMPDIR="$BASE" "$RUN_QEMU" --background --timeout 60 "$DUMMY" \
    >"$OUTD/o.out" 2>"$OUTD/o.err" &
rpid=$!
owner=""
if wait_guest_up; then
    for pid in $(pgrep -f -- "$BASE" 2>/dev/null); do
        [[ "$(cat "/proc/$pid/comm" 2>/dev/null)" == timeout ]] \
            && owner=$(awk '{print $4}' "/proc/$pid/stat" 2>/dev/null)
    done
fi
if [[ -z "$owner" ]] || grep -q '^QEMU_PID=' "$OUTD/o.out"; then
    say_fail "control: no owner found before the handover (owner='${owner}'), so nothing below means anything"
else
    kill -TERM "$owner" 2>/dev/null
    wait_no_guest 10 || say_fail "the guest outlived a TERM of its owner before the handover ($(guests) still running)"
fi
wait "$rpid" 2>/dev/null
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh --background: a TERM to the owner before the handover ends the guest"

# --- 3. profile-qemu.sh, sent a TERM while it waits for the app to load ------
# The dummy app never loads, so the script sits in that wait for its whole
# --warmup; its own work dir comes from mktemp, so TMPDIR points it here.
f0=$fail
new_base
ptmp="$OUTD/ptmp"; mkdir -p "$ptmp"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
TMPDIR="$ptmp" AXL_QEMU_TMPDIR="$BASE" "$PROFILE_QEMU" --port "$port" \
    --warmup 60 --out "$OUTD/prof" "$DUMMY" >"$OUTD/p.out" 2>"$OUTD/p.err" &
ppid_=$!
waiting=0
for _ in $(seq 1 300); do
    if grep -q '^\[profile\] waiting for' "$OUTD/p.err" 2>/dev/null; then waiting=1; break; fi
    sleep 0.1
done
if [[ "$waiting" -ne 1 ]] || ! wait_guest_up; then
    say_fail "control: profile-qemu.sh never reached its warm-up wait with a guest up"
    sed 's/^/        /' "$OUTD/p.err" | head -6
fi
nkill=$(cat "$OUTD/p.err" | wc -l)
kill -TERM "$ppid_" 2>/dev/null
wait "$ppid_"; rc=$?
[[ "$rc" -eq 143 ]] || say_fail "a TERM did not end profile-qemu.sh by SIGTERM (status $rc, want 143)"
after=$(tail -n +"$(( nkill + 1 ))" "$OUTD/p.err" | grep '^\[profile\]')
if [[ -n "$after" ]]; then
    say_fail "profile-qemu.sh resumed after its cleanup and reported on its own kill:"
    printf '%s\n' "$after" | sed 's/^/        /'
fi
[[ -z "$(find "$ptmp" -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ]] \
    || say_fail "a TERM left profile-qemu.sh's work dir: $(ls "$ptmp")"
wait_no_guest 10 || say_fail "the guest outlived a TERM of profile-qemu.sh ($(guests) still running)"
[[ $fail -eq $f0 ]] && echo "  PASS: profile-qemu.sh sent TERM: dies by it, no false diagnosis, work dir gone, guest gone"

# --- 4. run-qemu.sh --screenshot, sent a TERM of its own -----------------------
# The screenshot guest is launched with a bare `&` and NO timeout watchdog --
# the script kills it itself after the capture. So a script that dies by a
# signal without killing it first leaves a guest nothing will ever stop. The
# TERM is handled once the script's current `sleep` returns (a signal to the
# script alone does not interrupt its foreground command), so SHOT_WAIT is
# kept short.
f0=$fail
new_base
SHOT_WAIT=4 AXL_QEMU_TMPDIR="$BASE" "$RUN_QEMU" --timeout 60 \
    --screenshot "$OUTD/shot.png" "$DUMMY" >"$OUTD/s.out" 2>"$OUTD/s.err" &
spid=$!
if ! wait_guest_up; then
    say_fail "control: run-qemu.sh --screenshot never showed a guest, so nothing below means anything"
fi
kill -TERM "$spid" 2>/dev/null
wait "$spid"; rc=$?
[[ "$rc" -eq 143 ]] || say_fail "a TERM did not end run-qemu.sh --screenshot by SIGTERM (status $rc, want 143)"
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a TERM left run-qemu.sh --screenshot's state dir: $(ls "$BASE")"
wait_no_guest 10 || say_fail "the --screenshot guest outlived a TERM of run-qemu.sh ($(guests) still running)"
[[ $fail -eq $f0 ]] && echo "  PASS: run-qemu.sh --screenshot sent TERM: dies by it, state dir gone, guest gone"

# --- 4b. run-qemu.sh --screenshot sent a TERM just after its guest's launch --
# Case 1c's window on the screenshot launch, where it costs more: this guest
# has no watchdog, so one the script leaves behind runs until killed by hand.
seam_term_case launch "just after its --screenshot guest's launch" --screenshot "$OUTD/shot2.png"

# --- 5. a common-test.sh test booting in the foreground, killed by the runner -
# test_run_foreground runs the guest under its own `timeout`, so the runner's
# group kill misses it exactly as it missed run-qemu.sh's -- and test_cleanup
# killed only a BACKGROUND guest. Measured before the fix: test-axl.sh killed
# at 5 s left its `timeout` and QEMU running, on a deleted disk, to the
# test's own QEMU timeout. The probe is the smallest such test: the dummy
# app, booted in the foreground for up to 60 s.
f0=$fail
new_base
cat > "$OUTD/test-fgprobe-qemu.sh" <<FGPROBE
#!/bin/bash
source "$SCRIPT_DIR/common-test.sh"
test_parse_args
test_setup
test_add_efi "$DUMMY" app.efi
printf 'fs0:\\\\app.efi\\n' | test_set_startup
test_build_image
test_build_qemu_cmd
test_run_foreground 60
FGPROBE
AXL_QEMU_TMPDIR="$BASE" AXL_TEST_TIMEOUT=12 timeout 12 bash "$OUTD/test-fgprobe-qemu.sh" \
    >"$OUTD/f.out" 2>&1 &
fpid=$!
if ! wait_guest_up; then
    say_fail "control: the foreground probe never showed a guest, so nothing below means anything"
    tail -5 "$OUTD/f.out" | sed 's/^/        /'
fi
wait "$fpid"; rc=$?
[[ "$rc" -eq 124 ]] || say_fail "the outer timeout did not fire on the foreground probe (status $rc)"
grep -qE '^KILLED: test-fgprobe-qemu\.sh was killed by the runner at its 12s time limit' "$OUTD/f.out" \
    || say_fail "the killed foreground probe did not report its kill"
wait_no_guest 10 || say_fail "the guest outlived a runner kill of a foreground test ($(guests) still running)"
[[ -z "$(ls -A "$BASE")" ]] || say_fail "a runner kill left the foreground test's scratch: $(ls "$BASE")"
[[ $fail -eq $f0 ]] && echo "  PASS: a foreground test killed by the runner: reports it, guest gone, scratch gone"

# --- 6. a bare `return` in a captured EXIT trap must not resume the script ---
# axl_die_by_signal (scripts/axl-common.sh) used to eval a captured EXIT trap
# command INLINE, in itself. A bare `return` inside that command then
# returned from axl_die_by_signal ITSELF -- skipping the kill below and
# letting the interrupted script resume, the exact bug class this whole file
# exists to fix, reopened one level down (review finding N-1). No shipped
# trap does this today; this pins the hardening -- CMD now runs through a
# nested wrapper function, so `return` inside it returns only from that --
# so a future trap that does still dies by the signal instead of resuming.
# No QEMU: this is a plain bash mechanism, host-only.
f0=$fail
rmark="$OUTD/return-mark"; mkdir -p "$rmark"
cat > "$OUTD/return-probe.sh" <<RPROBE
#!/bin/bash
source "$PROJECT_DIR/scripts/axl-common.sh"
MARK="\$1"
trap 'echo ran >> "\$MARK/ran"; return' EXIT
axl_exit_on_signal INT TERM
echo READY
( sleep 30 )
echo RESUMED >> "\$MARK/ran"
RPROBE
chmod +x "$OUTD/return-probe.sh"
timeout 10 "$OUTD/return-probe.sh" "$rmark" >"$OUTD/return-probe.out" 2>&1 &
rpid=$!
rready=0
for _ in $(seq 1 50); do grep -q READY "$OUTD/return-probe.out" 2>/dev/null && { rready=1; break; }; sleep 0.1; done
[[ "$rready" -eq 1 ]] \
    || say_fail "control: the return-trap probe never signalled ready, so the checks below prove nothing"
kill -INT -- "-$rpid"
wait "$rpid"; rc=$?
[[ "$rc" -eq 130 ]] \
    || say_fail "a bare return in a captured EXIT trap did not still die by SIGINT (status $rc, want 130)"
grep -qx RESUMED "$rmark/ran" 2>/dev/null \
    && say_fail "a bare return in a captured EXIT trap RESUMED the script"
[[ "$(grep -cx ran "$rmark/ran" 2>/dev/null)" -eq 1 ]] \
    || say_fail "the return-trap's cleanup did not run exactly once"
[[ $fail -eq $f0 ]] && echo "  PASS: a bare return in a captured EXIT trap still dies by the signal, cleanup runs once, no resume"

# --- 7. `exit $?` in a captured EXIT trap must die with the signal's own
# code, not 0 ------------------------------------------------------------------
# `$?` at eval time used to be whatever axl_die_by_signal's own prior command
# left it as (0 -- the checks before the eval all succeed), not the signal
# death this function reports, so a trap ending in a bare `exit $?` exited 0
# instead of dying by the signal (review finding N-1, probe G). A subshell
# exit right before the eval now presets $? to the signal's conventional
# 128+n first. No shipped trap does this today.
f0=$fail
emark="$OUTD/exit-mark"; mkdir -p "$emark"
cat > "$OUTD/exit-probe.sh" <<EPROBE
#!/bin/bash
source "$PROJECT_DIR/scripts/axl-common.sh"
MARK="\$1"
trap 'exit \$?' EXIT
axl_exit_on_signal INT TERM
echo READY
( sleep 30 )
echo RESUMED >> "\$MARK/ran"
EPROBE
chmod +x "$OUTD/exit-probe.sh"
timeout 10 "$OUTD/exit-probe.sh" "$emark" >"$OUTD/exit-probe.out" 2>&1 &
epid=$!
eready=0
for _ in $(seq 1 50); do grep -q READY "$OUTD/exit-probe.out" 2>/dev/null && { eready=1; break; }; sleep 0.1; done
[[ "$eready" -eq 1 ]] \
    || say_fail "control: the exit-status-trap probe never signalled ready, so the checks below prove nothing"
kill -INT -- "-$epid"
wait "$epid"; rc=$?
[[ "$rc" -eq 130 ]] \
    || say_fail "exit \$? in a captured EXIT trap did not die with SIGINT's own code (status $rc, want 130)"
grep -qx RESUMED "$emark/ran" 2>/dev/null \
    && say_fail "exit \$? in a captured EXIT trap RESUMED the script"
[[ $fail -eq $f0 ]] && echo "  PASS: exit \$? in a captured EXIT trap dies with the signal's own code, not 0"

echo
if [[ $fail -eq 0 ]]; then echo "signal cleanup selftest: OK"; else echo "signal cleanup selftest: FAILED"; exit 1; fi
