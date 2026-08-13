#!/bin/bash
# test-run-qemu-bg-leak-selftest.sh — regression test for run-qemu.sh
# --background cleanup ownership.
#
# --background delegates cleanup to a detached subshell. Two failure shapes,
# both observed in the field as multi-day /dev/shm leaks:
#
#   A. SIGTERM the cleanup-owner subshell. The `timeout` watchdog is a CHILD
#      of that subshell, but orphans do not die with their parent: it survives,
#      fires, and reaps the guest correctly. What is lost is the trailing
#      `rm -rf "$TMPDIR"` — the last command of a killable subshell — so ~42 MB
#      of state dir leaks with nothing holding it.
#   B. SIGKILL the owner AND the watchdog. Nothing is left to bound the guest's
#      lifetime, so it survives forever at ppid 1. That shape is SELF-PROTECTING:
#      the stale-dir sweeper skips any dir a live process references, and the
#      immortal guest carries the path in argv, so the sweeper can never collect
#      it.
#
# Real guests, so this needs QEMU + OVMF — but it is deliberately NOT part of
# the integration matrix. The assertions are `df -k /dev/shm` deltas, and a df
# delta is a GLOBAL measurement: under the parallel pool, sibling guests churn
# 40 MB state dirs concurrently and the number becomes noise. Excluded from
# discovery by name (*-selftest.sh), exactly as test-runner-selftest.sh is.
# Run it by hand.
#
# Two traps this test is built to avoid, both paid for once already:
#
#   1. `pgrep -f` matches any process merely MENTIONING a path, including the
#      harness. So every path here is generated at runtime with mktemp, and the
#      probe is never handed a literal.
#   2. A previous attempt filtered its own pgrep self-matches with a token that
#      was a PREFIX of the mktemp base — so the filter ate the guest too and the
#      "held" count was meaningless in every run. This version uses NO filter.
#      Instead it takes a positive control (`held` must be >= 1 while the guest
#      is up, and must name a qemu-system process) before asserting it drops to
#      0. A probe that cannot see the guest fails the positive control rather
#      than silently reporting a clean run.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

# Guest lifetime. Long enough that the watchdog cannot fire before the harness
# has taken its measurements (run-qemu itself spends ~2 s in its hostfwd probe
# before returning), short enough that the whole file stays under a minute.
GUEST_TIMEOUT="${BG_LEAK_TIMEOUT:-12}"
# df noise floor, same value test-runner-selftest.sh uses. The leak under test
# is ~42 MB, so this discriminates by a factor of five.
NOISE_KB=8192

fail=0
case_fail=0
BASE=""; TMPD=""; HANDLE=""; OWNER=""; GUEST=""

DUMMY="$(mktemp --suffix=.efi)"

# Never leave our own strays behind, pass or fail. Two handles: the guest
# carries the state dir in argv, and the state dir lives under the base.
harness_cleanup() {
    [[ -n "$GUEST"  ]] && kill -9 "$GUEST"  2>/dev/null
    [[ -n "$HANDLE" ]] && kill -9 "$HANDLE" 2>/dev/null
    [[ -n "$OWNER"  ]] && kill -9 "$OWNER"  2>/dev/null
    if [[ -n "$BASE" ]]; then
        pkill -9 -f -- "$BASE" 2>/dev/null
        sleep 0.3
        rm -rf "$BASE"
    fi
    rm -f "$DUMMY"
}
trap harness_cleanup EXIT

df_used() { df -k --output=used /dev/shm | tail -1 | tr -d ' '; }

# Processes referencing the current guest's state dir. No self-filter by
# design — see the header. `wc -l` never receives the path, and pgrep excludes
# its own pid, so nothing here can match itself.
held_count() { pgrep -af -- "$TMPD" 2>/dev/null | wc -l; }

orphan_qemus() { ps -eo pid,ppid,comm | awk '$2==1 && $3 ~ /^qemu-system/' | wc -l; }

say_fail() { echo "  FAIL: $*"; fail=1; case_fail=1; }

# Launch one --background guest into a fresh base and resolve the process
# shape. Returns non-zero (and reports) if the shape is not what the
# assertions below depend on, so a structural change to run-qemu.sh fails
# loudly instead of silently measuring nothing.
bg_launch() {
    local out rc=0
    BASE=$(mktemp -d -p /dev/shm axlbgprobe.XXXXXXXX)
    out=$(AXL_QEMU_TMPDIR="$BASE" "$RUN_QEMU" --background \
              --timeout "$GUEST_TIMEOUT" "$DUMMY" 2>/dev/null) || rc=$?
    HANDLE=$(sed -n 's/^QEMU_PID=//p' <<<"$out" | head -1)
    TMPD=$(sed -n 's/^TMPDIR=//p' <<<"$out" | head -1)
    if [[ -z "$HANDLE" || -z "$TMPD" ]]; then
        say_fail "run-qemu --background did not report QEMU_PID/TMPDIR (rc=$rc)"
        return 1
    fi
    # QEMU_PID is the `timeout` watchdog, NOT qemu: GNU timeout fork+execs, so
    # the guest is a grandchild of the cleanup-owner subshell.
    OWNER=$(ps -o ppid= -p "$HANDLE" 2>/dev/null | tr -d ' ')
    GUEST=$(pgrep -P "$HANDLE" 2>/dev/null | head -1)
    local hcomm gcomm
    hcomm=$(ps -o comm= -p "$HANDLE" 2>/dev/null)
    gcomm=$(ps -o comm= -p "${GUEST:-0}" 2>/dev/null)
    if [[ "$hcomm" != "timeout" ]]; then
        say_fail "QEMU_PID $HANDLE is '$hcomm', expected the timeout watchdog"
        return 1
    fi
    if [[ "$gcomm" != qemu-system* ]]; then
        say_fail "watchdog child ${GUEST:-none} is '$gcomm', expected qemu-system*"
        return 1
    fi
    if [[ -z "$OWNER" || "$OWNER" == "1" ]]; then
        say_fail "no cleanup-owner subshell above the watchdog (ppid=$OWNER)"
        return 1
    fi
    return 0
}

# The positive control for held_count: while the guest is up the probe MUST see
# it, and MUST see it as a qemu-system process. Without this a broken probe
# would report every run clean.
probe_discriminates() {
    local n; n=$(held_count)
    if [[ "$n" -lt 1 ]]; then
        say_fail "probe blind: nothing references the live guest's state dir"
        return 1
    fi
    if ! pgrep -af -- "$TMPD" 2>/dev/null | grep -q 'qemu-system'; then
        say_fail "probe blind: no qemu-system process among the $n references"
        return 1
    fi
    return 0
}

# Wait for a pid to disappear, up to N deciseconds. 0 = still alive.
wait_gone() {
    local pid="$1" tries="$2"
    for _ in $(seq 1 "$tries"); do
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 0
}

reset_state() { BASE=""; TMPD=""; HANDLE=""; OWNER=""; GUEST=""; }

# --- Case 1: an undisturbed --background run is unchanged -------------------
# The guard against the pdeathsig fix firing early. pdeathsig ties the guest's
# life to the `timeout` watchdog, which lives the guest's full lifetime in
# normal operation — but if that reasoning were wrong the guest would die
# seconds after launch, which is a worse regression than the leak. This case
# also pins the self-clean semantics the trap must preserve: the state dir goes
# away when the guest does, not before.
case_normal() {
    local before after delta held
    case_fail=0
    before=$(df_used)
    bg_launch || { reset_state; return; }
    probe_discriminates || { reset_state; return; }

    sleep 3
    if ! kill -0 "$GUEST" 2>/dev/null; then
        say_fail "guest died ~3 s into a ${GUEST_TIMEOUT}s undisturbed run (pdeathsig firing early?)"
        reset_state; return
    fi
    [[ -d "$TMPD" ]] || say_fail "state dir removed while the guest was still running"

    if wait_gone "$GUEST" $(( (GUEST_TIMEOUT + 20) * 10 )); then
        say_fail "watchdog never reaped the guest"
        reset_state; return
    fi
    sleep 1
    [[ -e "$TMPD" ]] && say_fail "undisturbed run left its state dir: $TMPD"
    held=$(held_count)
    [[ "$held" -ne 0 ]] && say_fail "undisturbed run left $held process(es) holding $TMPD"
    after=$(df_used); delta=$(( after - before ))
    [[ "$delta" -gt "$NOISE_KB" ]] && say_fail "undisturbed run leaked ${delta} KB of /dev/shm"

    rm -rf "$BASE"; reset_state
    [[ "$case_fail" -eq 0 ]] && echo "  PASS: undisturbed --background run lives its full timeout, then self-cleans"
}

# --- Case 2 (shape A): SIGTERM the cleanup-owner subshell -------------------
case_shape_a() {
    local before after delta held pre_orphans post_orphans
    case_fail=0
    before=$(df_used); pre_orphans=$(orphan_qemus)
    bg_launch || { reset_state; return; }
    probe_discriminates || { reset_state; return; }

    kill -TERM "$OWNER" 2>/dev/null

    # Cleanup must stay tied to the guest's real lifetime: a trap that rm'd the
    # state dir the instant the signal arrived would pull the disk out from
    # under a live guest. Bash defers the handler until the foreground
    # `timeout` returns, and this is what pins that.
    sleep 2
    kill -0 "$GUEST" 2>/dev/null || say_fail "shape A: guest died with the owner (watchdog lost)"
    [[ -d "$TMPD" ]] || say_fail "shape A: state dir rm'd while the guest was still running"

    if wait_gone "$GUEST" $(( (GUEST_TIMEOUT + 20) * 10 )); then
        say_fail "shape A: orphaned watchdog never reaped the guest"
        reset_state; return
    fi
    sleep 1
    [[ -e "$TMPD" ]] && say_fail "shape A: state dir survived the killed owner: $TMPD"
    held=$(held_count)
    [[ "$held" -ne 0 ]] && say_fail "shape A: $held process(es) still hold $TMPD"
    after=$(df_used); delta=$(( after - before ))
    [[ "$delta" -gt "$NOISE_KB" ]] && say_fail "shape A: leaked ${delta} KB of /dev/shm"
    post_orphans=$(orphan_qemus)
    [[ "$post_orphans" -gt "$pre_orphans" ]] \
        && say_fail "shape A: ppid-1 qemu orphans $pre_orphans -> $post_orphans"

    rm -rf "$BASE"; reset_state
    [[ "$case_fail" -eq 0 ]] && echo "  PASS: shape A (SIGTERM owner) reclaims the state dir"
}

# --- Case 3 (shape B): SIGKILL the owner AND the watchdog ------------------
# Nothing survives SIGKILL to run an rm, so the state dir is EXPECTED to remain
# here. That is not the defect. The defect is the immortal guest: while it
# lives it holds the path in argv, and the sweeper's pgrep guard then skips the
# dir forever. Kill the guest with its watchdog and the leak stops protecting
# itself, which is all this case asserts.
case_shape_b() {
    local before after delta held pre_orphans post_orphans
    case_fail=0
    before=$(df_used); pre_orphans=$(orphan_qemus)
    bg_launch || { reset_state; return; }
    probe_discriminates || { reset_state; return; }
    local guest="$GUEST" tmpd="$TMPD"

    kill -KILL "$OWNER" "$HANDLE" 2>/dev/null

    if wait_gone "$guest" 100; then
        local ppid; ppid=$(ps -o ppid= -p "$guest" 2>/dev/null | tr -d ' ')
        say_fail "shape B: guest $guest outlived its watchdog (ppid=$ppid) — immortal orphan"
        kill -9 "$guest" 2>/dev/null; sleep 0.5
    fi
    held=$(held_count)
    [[ "$held" -ne 0 ]] && say_fail "shape B: $held process(es) still hold $tmpd — sweeper stays blind"
    post_orphans=$(orphan_qemus)
    [[ "$post_orphans" -gt "$pre_orphans" ]] \
        && say_fail "shape B: ppid-1 qemu orphans $pre_orphans -> $post_orphans"

    # The sweeper's job, done here so the test is self-contained. The df check
    # AFTER our own rm is the deleted-inode check: if anything were still
    # holding the disk, tmpfs would not give the pages back.
    rm -rf "$BASE"; sleep 0.5
    after=$(df_used); delta=$(( after - before ))
    [[ "$delta" -gt "$NOISE_KB" ]] \
        && say_fail "shape B: ${delta} KB of /dev/shm unreclaimed after removing the dir"

    reset_state
    [[ "$case_fail" -eq 0 ]] && echo "  PASS: shape B (SIGKILL owner+watchdog) leaves no immortal guest"
}

case_normal
case_shape_a
case_shape_b

[[ $fail -eq 0 ]] && echo "run-qemu bg-leak selftest: OK" \
                  || { echo "run-qemu bg-leak selftest: FAILED"; exit 1; }
