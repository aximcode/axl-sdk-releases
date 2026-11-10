#!/bin/bash
# run-selftests.sh -- the harness self-tests, OUTSIDE the suite's pool and on
# a schedule each file declares (see THE SCHEDULE below): what measures the host
# runs alone, what shares a ctest tree runs in one lane, the rest in parallel.
#
# WHAT THEY COVER. The `test/integration/test-*-selftest.sh` files check the
# test harness itself rather than the library -- the glob below finds them,
# so this sentence carries no count to go stale: run-integration.sh's worker pool,
# per-test timeout, signal handling, orphan reaping and est-vs-limit gate, and
# that a killed test reports the kill and still runs its cleanup;
# lib/discover.sh's metadata parsing and its arch/local-only filters; the port
# allocator's concurrent claims; run-qemu.sh's background-mode /dev/shm leak;
# and that a signal ends run-qemu.sh and profile-qemu.sh after their cleanup,
# and that a kill of either, or of a test booting in the foreground, takes the
# guest with it; and the gate runners' scoping flags (verify.sh --apps,
# run-integration.sh --for) -- that each NARROWS what runs and ANNOUNCES that
# it did -- and the lint gate's refusal to be scoped. Assertions nothing else
# makes.
#
# COST. ~265 s wall for ~355 s of work, scheduled (2026-11-08; ~350 s when it
# ran serially, 444 s in CI). Each verdict prints its file's time, and the
# summary line the wall against the work. The tree lane is the critical path:
# test-runner-selftest.sh is the dearest (~140 s): it drives hangs, retries
# and SIGTERM reaps on purpose. Then test-duration-budget-selftest.sh (~72 s,
# real sleeps against whole-second budgets) and test-gate-scoping-selftest.sh
# (~50 s). test-lint-gate-selftest.sh invokes the real scripts/lint.sh, the
# incremental x64-lint preset -- ~9 s over a warm build/x64-lint, ~70 s from
# cold, which is every CI run (checkout wipes the tree) -- so it runs in
# parallel rather than in the tree lane it once lengthened as part of
# gate-scoping. It SKIPS, loudly, where cmake, ninja or clang-tidy is absent --
# this script runs in CI's integration job, whose image need not carry them.
#
# WHY THEY ARE NOT DISCOVERED TESTS. lib/discover.sh excludes
# `test-*-selftest.sh` by name -- from the runner AND from check-test-meta --
# and that exclusion is correct: a self-test of the runner must not be
# scheduled BY the runner as a peer of the tests it is checking.
#
# WHY NOT A WRAPPER, EITHER -- this is the part that was learned expensively.
# A discovered wrapper (`test-selftests.sh`) that exec'd these four took the
# suite to 121 passed / 59 failed on 2026-09-05. Not contention:
# test-runner-selftest.sh's SIGTERM case deliberately drives a hang and then
# reaps it, and inside the suite's own six workers the reap did not complete --
# it left TWO qemu-system processes running and leaked 108 MB of /dev/shm, and
# those orphans then competed with every remaining test. A wrapper is a nested
# pool with an extra file; the exclusion's reason applied to it unchanged.
#
# SO: OUTSIDE THE POOL. This script is not `test-` prefixed, so
# discovery never sees it. CI runs it as its own step in the integration job,
# AFTER the suite -- deliberately after, so that if the SIGTERM case does fail
# and orphan something, it cannot poison the suite's results the way it did
# when it ran alongside them.
#
# WHY THEY CAN NOW SHARE THE BOX WITH EACH OTHER (2026-11-08; they ran
# serially until then, ~350 s locally and 444 s in CI). What made them
# unshareable was GLOBAL measurement: `df -k /dev/shm` deltas and host-wide
# orphan counts, which any neighbour moved. Those now list the processes
# holding a file under the test's OWN base (test/integration/lib/shm-holders.sh
# -- the only way tmpfs keeps a deleted file's pages) and count only guests
# that name it, each with a positive control. What still cannot share is
# declared per file (exclusive, tree) rather than assumed of all of them. The
# suite is still a different matter: its six workers churn guests the runner
# self-test's reap cases would race, so CI still runs this AFTER the suite.
#
# Usage: scripts/run-selftests.sh
set -uo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTDIR="$SCRIPT_DIR/../test/integration"

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== harness self-tests ==="
echo ""

# What exists BEFORE they run, so the check at the end reports only what they
# left: the guests and temp bases of both runners -- the old runner's
# axl-itest.* and a ctest run's axl-ct-* (whose base outlives a hard-killed
# run until the tree's next run takes it over, and belongs to whichever tree
# made it -- including CI's, on this box). A glob alone would report those.
# A base is keyed by name AND inode: a ctest base's name is the same every run
# (axl-ct-<tree tag>), so a name alone would hide one the self-tests leaked
# under a name a leftover already had.
_strays() {   # -> one line per guest pid and per temp base
    # [q]: the pattern never matches a shell whose own command line holds it.
    # A match counts only when the process IS QEMU (its comm): a status loop
    # or a grep whose command line merely names the pattern is not a guest.
    local p
    for p in $(pgrep -f '[q]emu-system.*/axl-(itest\.|ct-)' 2>/dev/null); do
        [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == qemu-system* ]] && echo "pid $p"
    done
    find /dev/shm /tmp -maxdepth 1 \( -name 'axl-itest.*' -o -name 'axl-ct-*' \) \
        -printf 'dir %p %i\n' 2>/dev/null
}
_new_strays() { comm -13 <(printf '%s\n' "$1") <(_strays | sort) | sed '/^$/d'; }
# THE DETECTOR PROVES IT CAN SEE, every run: a base planted between two
# snapshots must be reported. A check that cannot fail reads as "clean".
_probe_before=$(_strays | sort)
_probe=$(mktemp -d -p /dev/shm axl-ct-strayprobe.XXXXXX 2>/dev/null || mktemp -d -p /tmp axl-ct-strayprobe.XXXXXX)
_seen=$(_new_strays "$_probe_before" | grep -c -- "$_probe" || true)
rmdir "$_probe"
if [[ "$_seen" != 1 ]]; then
    echo "run-selftests: the stray detector did not see a temp base planted under its nose" \
         "($_probe) -- its \"nothing left behind\" would mean nothing" >&2
    exit 1
fi
# ...AND IT SEES ONLY GUESTS: a process whose command line merely CONTAINS the
# pattern is not one. A status loop that greps for /dev/shm/axl-itest.* beside
# `pgrep -c qemu-system` was reported as a leaked guest (2026-11-07). The decoy
# below carries the pattern in its argv and must not be reported.
_decoy_before=$(_strays | sort)
bash -c 'sleep 30; :' qemu-system-decoy /axl-ct-decoy >/dev/null 2>&1 &
_decoy=$!
sleep 0.2
_seen=$(_new_strays "$_decoy_before" | grep -c "^pid $_decoy\$" || true)
kill "$_decoy" 2>/dev/null
wait "$_decoy" 2>/dev/null
if [[ "$_seen" != 0 ]]; then
    echo "run-selftests: the stray detector reported a process that only NAMES a guest" \
         "(pid $_decoy, comm $(cat /proc/$_decoy/comm 2>/dev/null || echo bash)) --" \
         "its \"left behind\" would accuse status loops, not guests" >&2
    exit 1
fi
# ...WHILE STILL SEEING ONE: a process whose comm IS qemu-system* (bash run
# through a link of that name -- not `sleep`, which reads the pattern as a
# bad duration and exits at once) with the pattern in its argv must be
# reported -- or the comm filter above could pass by rejecting everything.
_fake_dir=$(mktemp -d)
ln -s "$(command -v bash)" "$_fake_dir/qemu-system-x86_64"
"$_fake_dir/qemu-system-x86_64" -c 'sleep 30; :' qemu /axl-ct-strayprobe >/dev/null 2>&1 &
_fake=$!
sleep 0.2
_seen=$(_new_strays "$_decoy_before" | grep -c "^pid $_fake\$" || true)
kill "$_fake" 2>/dev/null
wait "$_fake" 2>/dev/null
rm -f "$_fake_dir/qemu-system-x86_64"
rmdir "$_fake_dir"
if [[ "$_seen" != 1 ]]; then
    echo "run-selftests: the stray detector did not see a qemu-system process planted" \
         "under its nose (pid $_fake) -- its \"no stray guests\" would mean nothing" >&2
    exit 1
fi
STRAYS_BEFORE=$(_strays | sort)

# A SELF-TEST MUST DIE BY INT AND TERM. A bash trap handler that does not exit
# RESUMES the script, so `trap 'cleanup' EXIT INT TERM` turns a TERM into "clean
# up, then carry on": test-gate-scoping-selftest.sh kept running -- starting new
# work -- for as long as anyone waited, after this script's interrupt handler
# TERMed it (2026-11-08). In parallel that is every lane left running until its
# 300 s timeout. Cleanup belongs on EXIT, which still runs when the signal kills
# the script; INT/TERM handlers must end in an exit.
_traps_resume() {   # prints "line: text" for each INT/TERM trap with no exit
    local n=0 line h sigs re
    # `trap` where a command can start (an indented line, or after ; & | { (
    # then do), an optional `--`, a handler (quoted or a bare word), then a
    # signal list that names INT or TERM (SIGINT, SIGTERM, 2, 15) ANYWHERE.
    re="(^[[:space:]]*|[;&|{(][[:space:]]*|then[[:space:]]+|do[[:space:]]+)trap[[:space:]]+(--[[:space:]]+)?('[^']*'|\"[^\"]*\"|[^[:space:]'\"]+)[[:space:]]+([A-Z0-9 ]+)"
    while IFS= read -r line; do
        n=$((n + 1))
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        [[ "$line" =~ $re ]] || continue
        h="${BASH_REMATCH[3]}"
        [[ "$h" == [\'\"]* ]] && h="${h:1:${#h}-2}"
        sigs=" ${BASH_REMATCH[4]} "
        [[ "$sigs" =~ [[:space:]](SIG)?(INT|TERM|2|15)[[:space:]] ]] || continue
        # A lone function name is judged by ITS body, found in the same file
        # (`name() {` to the next line that is just `}`, or the one line of a
        # `name() { ...; }`); a function this file does not define cannot be
        # shown to exit, so it is flagged. Heredoc fixture lines are read as
        # the file's own traps -- a self-test that needs a resuming trap as a
        # FIXTURE must build that line at runtime.
        if [[ "$h" =~ ^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*$ ]]; then
            h=$(awk -v f="${BASH_REMATCH[1]}" '
                !on && $0 ~ "^[[:space:]]*" f "[[:space:]]*\\(\\)" {
                    print; if ($0 ~ /}[[:space:]]*$/) exit; on = 1; next }
                on { print } on && /^[[:space:]]*}[[:space:]]*$/ { exit }' "$1")
        fi
        [[ "$h" =~ (^|[^[:alnum:]_])exit([^[:alnum:]_]|$) ]] && continue
        echo "$n: $line"
    done < "$1"
}
# The check's own control: each resuming shape must be flagged, each exiting
# one not -- a check that cannot fail reads as "clean".
_tprobe=$(mktemp)
cat > "$_tprobe" <<'TPROBE'
trap 'cleanup' EXIT INT TERM
trap 'exit 130' INT TERM
v=$(mktemp -d); trap 'rm -rf "$v"' EXIT INT TERM
trap 'cleanup' INT TERM HUP
trap "echo exiting" TERM
trap 'on_term' TERM
# trap 'cleanup' INT TERM
trap 'cleanup' EXIT
trap -- 'rm -f x' 15
trap cleanup EXIT INT TERM
    trap 'cleanup' INT TERM
trap 'cleanup' SIGTERM
trap quick TERM
trap on_term SIGINT
cleanup() {
    rm -f x
}
on_term() {
    cleanup
    exit 143
}
quick() { cleanup; }
later() {
    exit 1
}
TPROBE
_twant=$'1: trap \'cleanup\' EXIT INT TERM\n3: v=$(mktemp -d); trap \'rm -rf "$v"\' EXIT INT TERM\n4: trap \'cleanup\' INT TERM HUP\n5: trap "echo exiting" TERM\n9: trap -- \'rm -f x\' 15\n10: trap cleanup EXIT INT TERM\n11:     trap \'cleanup\' INT TERM\n12: trap \'cleanup\' SIGTERM\n13: trap quick TERM'
if [[ "$(_traps_resume "$_tprobe")" != "$_twant" ]]; then
    echo "run-selftests: the trap check's control failed -- it flagged:" >&2
    _traps_resume "$_tprobe" | sed 's/^/    /' >&2
    rm -f "$_tprobe"
    exit 1
fi
rm -f "$_tprobe"

# A GLOB, never a written-out list. A fifth self-test must be picked up without
# editing this file -- a hand-kept list here would recreate, one level up, the
# very defect this script exists to fix: assertions that exist and never run.
shopt -s nullglob
SELFTESTS=("$TESTDIR"/test-*-selftest.sh)
shopt -u nullglob

# An empty glob is not "nothing to do". It means the naming convention moved
# and this script has silently stopped covering anything -- which is exactly
# the state it was written to end.
if [[ "${#SELFTESTS[@]}" -eq 0 ]]; then
    fail "no test-*-selftest.sh found under $TESTDIR -- the naming convention changed and this covers nothing"
    echo ""
    echo "selftests: $PASS passed, $FAIL failed"
    exit 1
fi

# THE SCHEDULE IS DECLARED BY EACH FILE, not listed here (the glob rule
# above): a `# selftest-schedule:` header line says what a file cannot share.
#   exclusive  runs ALONE, first: it measures the host itself
#              (test-duration-budget-selftest.sh samples live load and asserts
#              an idle box reads x1.00, so any neighbour is in its number).
#   tree       runs in ONE serial lane beside the parallel set: these use
#              build/x64-debug's ctest registration -- the runner's self-test
#              drives stub runs that share one owner record and temp base
#              there (integration-logs-stub), plants a fake owner on purpose,
#              and can RECONFIGURE the tree, which gate-scoping reads
#              (ctest --test-dir, verify.sh --print-ctest).
#   (none)     runs in parallel with everything else in phase 2.
# A new file is parallel by default -- so a new self-test that measures
# something GLOBAL must say so, or it will see its neighbours. Read from the
# LEADING comment block only: a fixture line further down that happens to
# start the same way (a self-test of this scheduler would have one) is not a
# declaration.
EXCLUSIVE=(); TREE_LANE=(); PARALLEL=()
declare -A SCHEDULED=()
for st in "${SELFTESTS[@]}"; do
    name="$(basename "$st")"
    if [[ ! -x "$st" ]]; then
        fail "$name is not executable"
        continue
    fi
    if _resuming=$(_traps_resume "$st") && [[ -n "$_resuming" ]]; then
        fail "$name traps INT/TERM without exiting -- a TERM resumes it (cleanup goes on EXIT):"
        sed 's/^/      /' <<<"$_resuming"
    fi
    sched=$(awk 'NR > 1 && !/^#/ { exit }
                 /^# selftest-schedule:/ { sub(/^# selftest-schedule: */, "");
                                           sub(/[^a-z].*$/, ""); print; exit }' "$st")
    case "$sched" in
        exclusive) EXCLUSIVE+=("$st") ;;
        tree)      TREE_LANE+=("$st") ;;
        "")        PARALLEL+=("$st") ;;
        *)         fail "$name: unknown selftest-schedule '$sched' (exclusive|tree)"; continue ;;
    esac
    SCHEDULED[$name]=1
done

# One file: bounded (a self-test that wedges must not wedge CI), its output,
# status and wall time kept under $RES for the report below. `timeout` puts
# itself in a PROCESS GROUP OF ITS OWN, so a signal to this script, or to its
# group, never reaches it or the self-test under it -- only a TERM sent to the
# timeout itself does (it forwards it). So each lane knows its CURRENT timeout
# (_cur_tp) and forwards a TERM to it (_lane_term); $RES/NAME.live names the
# running ones, for the interrupt report, and goes when the file ends -- a pid
# kept past its process's life could be reused by an unrelated one.
RES=$(mktemp -d)
_cur_tp=""; _bang0=""
run_one() {
    local st="$1" name t0 rc=0
    name="$(basename "$st")"
    t0=$SECONDS
    _bang0=${!:-}
    timeout 300 "$st" > "$RES/$name.log" 2>&1 &
    _cur_tp=$!
    echo "$_cur_tp" > "$RES/$name.live"
    wait "$_cur_tp" || rc=$?
    # Both cleared: $! is this finished timeout now, and a later signal must
    # not read it as a launch in flight (_term_current) and TERM a reaped pid.
    _cur_tp=""; _bang0=${!:-}
    rm -f "$RES/$name.live"
    echo "$rc $((SECONDS - t0))" > "$RES/$name.res"
}
# TERM this lane's current timeout -- or, for a signal between the `&` and the
# assignment, $! when it is not the job that was newest before ($_bang0).
_term_current() {
    local t="$_cur_tp"
    [[ -z "$t" && "${!:-}" != "$_bang0" ]] && t=$!
    [[ -n "$t" ]] && kill -TERM "$t" 2>/dev/null
    return 0
}
_lane_term() { _term_current; exit 143; }
run_lane() { local st; for st in "$@"; do run_one "$st"; done; }
# AN INTERRUPT ENDS EVERY LANE, not just this script. Serially a signal could
# orphan one file; in parallel it would orphan every running file and its
# guests, each until its own 300 s timeout. TERM the foreground lane's timeout
# and every background lane (each forwards to its own), then wait -- bounded --
# and SAY which self-tests are still finishing: one deep in a command of its
# own (lint-gate's lint.sh runs under a timeout of ITS own, which no TERM
# from here reaches) defers its exit until that command returns.
pids=()
_selftests_interrupted() {
    local sig="$1" f p left=""
    trap - INT TERM
    _term_current
    for p in ${pids[@]+"${pids[@]}"}; do kill -TERM "$p" 2>/dev/null; done
    for _ in $(seq 1 30); do
        left=""
        for f in "$RES"/*.live; do
            [[ -e "$f" ]] || continue
            p=$(cat "$f")
            [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == timeout ]] && left+=" $(basename "$f" .live)"
        done
        [[ -z "$left" ]] && break
        sleep 1
    done
    if [[ -z "$left" ]]; then
        echo "selftests: interrupted by SIG$sig -- every running self-test has ended" >&2
    else
        echo "selftests: interrupted by SIG$sig -- still finishing after 30 s (TERMed;" \
             "each ends when its current command returns):$left" >&2
    fi
    rm -rf "$RES"
    exit $((128 + $(kill -l "$sig")))
}
trap '_selftests_interrupted INT' INT
trap '_selftests_interrupted TERM' TERM

wall0=$SECONDS
run_lane ${EXCLUSIVE[@]+"${EXCLUSIVE[@]}"}
( trap _lane_term TERM; run_lane ${TREE_LANE[@]+"${TREE_LANE[@]}"} ) & pids+=($!)
for st in ${PARALLEL[@]+"${PARALLEL[@]}"}; do ( trap _lane_term TERM; run_one "$st" ) & pids+=($!); done
for p in "${pids[@]}"; do wait "$p"; done
wall=$((SECONDS - wall0))

# Verdicts in glob order, each with its wall time: this step was 444 s of CI's
# QEMU job with nothing saying which file spent it.
work=0
for st in "${SELFTESTS[@]}"; do
    name="$(basename "$st")"
    [[ -n "${SCHEDULED[$name]:-}" ]] || continue   # not executable / bad schedule: failed above
    if ! read -r rc secs < "$RES/$name.res" 2>/dev/null; then
        fail "$name was scheduled and left no result -- it never ran, or its runner died"
        continue
    fi
    work=$((work + secs))
    if [[ "$rc" -eq 0 ]]; then
        pass "$name ${secs}s"
        # A pass that SKIPPED something says so here: logs print only on a
        # failure, so a self-test whose tools were missing (lint-gate without
        # clang-tidy) otherwise read as a full pass.
        grep -E '^ *SKIP:' "$RES/$name.log" | sed 's/^ */      /' || true
    else
        fail "$name exited $rc after ${secs}s"
        sed 's/^/      /' "$RES/$name.log" | tail -20
    fi
done
rm -rf "$RES"
echo "  selftests: ${wall}s wall for ${work}s of self-test work" \
     "(${#EXCLUSIVE[@]} exclusive, ${#TREE_LANE[@]} in the tree lane, ${#PARALLEL[@]} parallel)"

# WHAT THEY LEAVE BEHIND IS PART OF THE RESULT. The failure that motivated this
# script was orphaned guests and a leaked tmpfs tree, and a run that reports
# "4 passed" while holding two qemu processes has not told the truth. Reported
# rather than cleaned: this is a diagnostic, and something that silently tidies
# up cannot tell you it had to.
new_strays=$(_new_strays "$STRAYS_BEFORE")
stray_q=$(grep -c '^pid ' <<< "$new_strays" || true)
stray_t=$(grep -c '^dir ' <<< "$new_strays" || true)
if [[ -z "$new_strays" ]]; then
    pass "no stray guests or temp trees left behind"
else
    fail "left behind: $stray_q qemu process(es), $stray_t temp tree(s)"
    while read -r _k _v _; do
        if [[ "$_k" == pid ]]; then ps -o pid=,args= -p "$_v" 2>/dev/null | cut -c1-160; else echo "$_v"; fi
    done <<< "$new_strays" | sed 's/^/      /' | head -8
fi

echo ""
echo "selftests: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
