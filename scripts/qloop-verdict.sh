#!/bin/bash
# qloop-verdict.sh -- THE QUIET SLOT: AxlTestLoop's driver idle-budget timing
# cases, judged alone on the box. Overview: docs/AXL-Build-System-Design.md
# §10.14; this header is the authority on the verdict.
#
# The one copy of this verdict. verify.sh's qloop job runs it (serially, after
# everything else) and CTest's gate.qloop runs it (RUN_SERIAL), so neither
# re-derives it (docs/AXL-Build-System-Design.md §10.8, step 6).
#
# Those timing cases need a vCPU that keeps running while it spins, so under
# a concurrent suite's load they void and SKIP -- the x64 and aa64 unit runs
# cannot judge them. This runs AxlTestLoop alone on x64 (KVM judges the logic;
# aa64 TCG is the noisy one), with no ratchet (a subset) and the hang timeout
# unscaled -- nothing runs beside it. Green needs POSITIVE evidence: the test
# prints the fixed line `LOOP-TIMING: judged` when it judged the group, and
# this reads PASS only with that line AND a clean run. A failure reads FAIL as
# usual. No failure and no marker -- every attempt voided, or anything else
# that left the group unjudged, however the test words it -- reads UNJUDGED:
# a timing gate that skipped is not one that passed.
#
# ON A LEASED GUEST when a pool serves (AXL_ONE_POOL_SOCK, which gate.qloop's
# registration sets as it does for unit.<App>): one `unit` guest through
# test/integration/lib/pool-or-boot.sh, a pool miss running it booted, so the
# slot pays no boot (docs/AXL-Test-Guest-Pool-Design.md §6.7 step 5). First
# it asks the pool to SETTLE (`axlpool settle`): a released guest resets in
# the background, and the two trees' last releases are still restoring when
# this starts -- the very load the slot exists to avoid. Bounded (60 s), and
# a pool still busy after it is named in the verdict rather than waited on
# forever. The witness below samples the leased guest's own QEMU, which the
# lease names (AXL_POOL_QEMU_PID). The verdict says where it ran.
#
# LOAD MUST NEVER READ AS A TIMING FAIL. The guest's noise probes sample at
# points, and load arriving between them twice failed a judged run (load
# 11-17). What covers which kind of noise:
#   - TRANSIENT noise (it spoils some attempts, not all): the guest rule
#     (axl-test-loop.c, IdleVerdict). A timing bound FAILS only if EVERY
#     judged attempt missed it, with at least two judged; the attempts stop
#     early only once every bound has held. A miss in the only judged
#     attempt, the others void, reads UNJUDGED and the verdict names every
#     such bound ("unconfirmed miss(es): <check>, ..."). It does NOT answer
#     sustained noise: a slowdown present in every attempt misses the same
#     bounds every time and reads exactly like a defect.
#   - SUSTAINED PREEMPTION: the guest's probes (a descheduled vCPU shows as
#     clock gaps, which void the attempt) and, here, an independent WITNESS:
#     the harness samples how long the guest's QEMU threads waited for a CPU
#     (schedstat run delay, TEST_VCPU_STARVE_FILE; read per thread, reading to
#     reading, so a thread that exits mid-run cannot cancel the run's wait --
#     vcpu_starve_sampler in axl-common.sh), and a FAIL that is
#     POSITIVELY only missed timing bounds (timing_only) while that exceeded
#     STARVE_MAX_MS reads UNJUDGED, with the figure. The witness sees this
#     kernel's scheduler only; a hypervisor below it that deschedules the
#     vCPU is seen by the probes, not by it.
#   - SUSTAINED SLOWDOWN WITHOUT PREEMPTION -- a vCPU moved to a slower
#     (E-)core, a frequency or thermal drop, SMT-sibling contention: NOT
#     COVERED. Nothing preempts, so neither the probes nor the witness see
#     it, and a renderer bound stretched in every attempt FAILs.
# Never relabelled, whatever the load: a functional check (the return value
# of an add, attach or defer) -- the guest prints its miss as "failed (not
# timing)", which the witness cannot match -- a leak, a stall, or any
# failure not named as a judged timing miss. The witness never touches a
# pass, and its figure is printed on every run. A real violation on a quiet
# box misses its bound in every attempt and waits ~0 ms for a CPU, so it
# still FAILs.
#
# Usage: scripts/qloop-verdict.sh [--dir DIR]
#   The harness's output is passed through (CTest shows it on a failure), and
#   the LAST line is the verdict: `qloop-verdict: <detail>`.
#   --dir DIR  write the run's files there (verify.sh's job directory) and
#              leave them; without it they go to a temp directory, removed on
#              a judged pass and kept -- and named -- otherwise.
# Exit: 0 judged and green; 3 UNJUDGED (gate.qloop's SKIP_RETURN_CODE);
#       anything else a FAIL. A harness that itself exits 3 is reported as 1,
#       so a failure can never be read as the skip.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Parsed BEFORE the cd below, so a relative --dir is the caller's.
dir="" own=0
case "$#:${1:-}" in
    2:--dir) dir="$2"
             [[ -d "$dir" ]] || { echo "qloop-verdict.sh: no directory $dir" >&2; exit 2; }
             dir=$(CDPATH='' cd "$dir" && pwd) || exit 2 ;;
    0:)      dir=$(mktemp -d -t axl-qloop.XXXXXX) || exit 2; own=1 ;;
    *)       echo "usage: $0 [--dir DIR]" >&2; exit 2 ;;
esac
cd "$(dirname "$SCRIPT_DIR")" || exit 2

APP=AxlTestLoop
MARK='LOOP-TIMING: judged'
# The independent witness (see the header): a timing FAIL while the guest's
# QEMU threads waited longer than this for a CPU is load, not a verdict. ONE
# limit, shared with test-net-latency-qemu.sh's witness: AXL_STARVE_MAX_MS in
# axl-common.sh, which says what it was measured against.
# shellcheck source=axl-common.sh
source "$SCRIPT_DIR/axl-common.sh" || exit 2
STARVE_MAX_MS=$AXL_STARVE_MAX_MS

log="$dir/qloop-run.log"
serial="$dir/qloop.serial"
# A --dir is reused (gate.qloop's is the tree's qloop-logs), and the harness
# copies its serial log there only if it made one: a previous run's marker
# must never stand as this run's evidence.
rm -f "$log" "$serial" "$dir/qloop.starve" "$dir/qloop-verdict.txt" "$dir/qloop-pool.log" \
      "$dir/qloop-run.booted.log"

sock="${AXL_ONE_POOL_SOCK:-}"
ran="booted (no pool)"
if [[ -n "$sock" ]]; then
    ran="on a leased guest"
    _settle=$(cd test/pool && timeout 90 python3 -m axlpool settle --socket "$sock" --timeout 60 2>&1)
    _src=$?
    echo "$_settle"
    # Not settled, or no answer: run anyway (a miss boots), and say so.
    # Named by its LAST line, one line: a traceback must not split the
    # verdict, which is this script's last line.
    _settle=$(printf '%s\n' "$_settle" | tr -d '\r' | sed '/^$/d' | tail -1)
    (( _src == 0 )) || ran="on a leased guest, the pool not settled (${_settle#axlpool settle: })"
fi

# The harness by ABSOLUTE path: a pooled attempt runs it from test/pool. Each
# run -- the pooled attempt and a miss's booted rerun -- first removes the
# serial log and the witness's file, so neither can carry the attempt's
# evidence into the rerun's verdict.
# To a file, then printed -- not through tee: a pipe is held open by anything
# the harness leaves behind (an orphaned QEMU), and tee would then wait on it
# after `timeout` had already returned. A file waits for nothing.
env TEST_APPS_ONLY="$APP" TEST_SKIP_RATCHET=1 \
    TEST_VCPU_STARVE_FILE="$dir/qloop.starve" \
    ./test/integration/lib/pool-or-boot.sh --class unit --arch X64 --tree "$PWD" \
        --log "$dir/qloop-pool.log" --timeout 300 \
        -- bash -c 'rm -f -- "$1" "$2" && exec "$3" --log "$1"' qloop-run \
           "$serial" "$dir/qloop.starve" "$PWD/test/integration/test-axl.sh" > "$log" 2>&1
rc=$?
cat "$log"
_miss=$(sed -n 's/^pool-or-boot: pool miss (\(.*\)); running it booted$/\1/p' "$log" | head -1)
# What is judged: the whole log, or after a miss ONLY the booted rerun's part
# -- the discarded attempt's Results, starvation figure (another QEMU's) and
# FAIL lines are not this verdict's.
vlog="$log"
if [[ -n "$_miss" ]]; then
    ran="booted after a pool miss ($_miss)"
    vlog="$dir/qloop-run.booted.log"
    sed -n '/^pool-or-boot: pool miss (/,$p' "$log" | tail -n +2 > "$vlog"
fi

# timing_only LOG SERIAL -- POSITIVELY, everything that failed is a timing
# bound a judged attempt missed (the guest names each "judged attempt N
# missed: <check>"), so the witness may speak to it. It must make the whole
# case, not fail to find a counter-example:
#   - every line of the job log carrying "FAIL" anywhere is exactly
#     "  FAIL: <check>" with <check> named as a judged miss -- so a leak
#     verdict ("*** FAIL: memory leaked ..."), or any failure printed some
#     other way, is not timing;
#   - no "***" harness verdict (leak, STALLED, TRUNCATED, CUT SHORT, a
#     firmware ASSERT) other than the skipped-groups notice;
#   - the last Results line's failed count equals the number of those FAIL
#     lines, so a failure no line names is not explained away.
# False when there is no FAIL line, or anything is left over.
timing_only() {
    local region fails missed f n nres verdicts
    # Only the guest's result region -- from the harness header on -- and
    # never a PASS or SKIP line: build output, or a check whose NAME says
    # FAIL, is not a failure, and reading it as one would turn load back
    # into a FAIL. No header, no case.
    # The harness's "*** N test GROUP(S) SKIPPED" notice re-lists every skip
    # reason below it, prefix stripped and indented: those lines are skip
    # reasons too, not failures, and are dropped with the notice's block.
    region=$(sed -n '/^=== AXL Integration Tests/,$p' "$1" 2>/dev/null | sed -E 's/\r$//' \
             | awk '/^\*\*\* [0-9]+ test GROUP\(S\) SKIPPED/ { skip = 1; print; next }
                    skip && /^    / { next }
                    { skip = 0; print }')
    [[ -n "$region" ]] || return 1
    fails=$(grep -a 'FAIL' <<<"$region" | grep -avE '^ *(PASS|SKIP(\[[0-9]+\])?): ' || true)
    [[ -n "$fails" ]] || return 1
    # Captured, not piped into grep -q: under pipefail an early-exiting
    # grep -q can SIGPIPE the writer and turn a match into a failure.
    verdicts=$(grep -a '\*\*\*' <<<"$region" \
               | grep -avE '^\*\*\* [0-9]+ test GROUP\(S\) SKIPPED' || true)
    [[ -z "$verdicts" ]] || return 1
    missed=$(grep -aoE 'judged attempt [0-9]+ missed: .*' "$2" 2>/dev/null \
             | sed -E 's/^judged attempt [0-9]+ missed: //; s/\r$//')
    n=0
    while IFS= read -r f; do
        [[ "$f" =~ ^\ *FAIL:\ (.*)$ ]] || return 1
        grep -qxF -- "${BASH_REMATCH[1]}" <<<"$missed" || return 1
        n=$((n + 1))
    done <<<"$fails"
    nres=$(grep -aE '^Results: ' <<<"$region" | tail -1 \
           | sed -nE 's/^Results: [0-9]+ passed, ([0-9]+) failed.*/\1/p')
    [[ "$nres" =~ ^[0-9]+$ ]] && (( nres == n ))
}

# The harness's own Results line, QUOTED rather than pattern-matched: echoing
# whatever it reports cannot go stale against its wording (verify.sh's table
# once printed a bare `see <log>` on every run because a matcher did).
results=$(grep -aE '^Results:' "$vlog" | tail -1 | sed 's/^Results: *//' || true)
if (( rc == 0 )); then
    verdict=0
    detail="$APP alone on x64 (quiet slot): $results"
else
    verdict=$rc
    (( verdict == 3 )) && verdict=1   # 3 is the SKIP; see the header
    if [[ -n "$results" ]]; then
        detail="FAILED: $results"
    else
        # The first line that looks like a diagnosis, truncated.
        _diag=$(grep -am1 -E 'FAIL|[Ee]rror|\*\*\*' "$vlog" | cut -c1-72 || true)
        detail="FAILED (exit $rc)${_diag:+: $_diag}"
    fi
fi

# The third outcome: it ran, failed nothing, and judged nothing. Decided by
# the ABSENCE of the test's fixed marker in its serial log, never by matching
# a skip's wording (which can change); the probe count, when the skip names
# one, is only the detail.
if (( verdict == 0 )) && ! grep -aqF "$MARK" "$serial" 2>/dev/null; then
    verdict=3
    _uj=$(grep -aoE '\(probe: [0-9]+ gaps' "$serial" "$vlog" 2>/dev/null \
            | head -1 | sed -E 's/.*\(probe: ([0-9]+) gaps/\1/')
    # A bound missed in the only judged attempt: name it, so a real defect on
    # a noisy box does not read exactly like pure noise.
    _um=$(grep -aoE 'LOOP-TIMING: unconfirmed miss: .*' "$serial" 2>/dev/null \
            | sed -E 's/^LOOP-TIMING: unconfirmed miss: //; s/\r$//' \
            | awk 'NR > 1 { printf ", " } { printf "%s", $0 }')
    _umn=$(grep -acE 'LOOP-TIMING: unconfirmed miss: ' "$serial" 2>/dev/null || true)
    if [[ -n "$_um" ]]; then
        _umw="unconfirmed miss"
        [[ "${_umn:-1}" -gt 1 ]] && _umw="unconfirmed misses"
        detail="UNJUDGED ($_umw: $_um) -- missed in the only judged attempt, the others void; re-run on a quiet box"
    elif [[ -n "$_uj" ]]; then
        detail="UNJUDGED (probe: $_uj gaps) -- no '$MARK' line; the box was not quiet"
    else
        detail="UNJUDGED -- no '$MARK' line in the serial log"
    fi
fi

# The witness. Read for every run and always printed, so a judged PASS says
# how quiet the box was too; it only ever turns a FAIL made entirely of missed
# timing bounds into UNJUDGED, never a pass into anything. Not measured is
# said, and changes nothing -- and only a non-negative figure is a figure.
_starve=$(grep -aoE '^vCPU starvation: [0-9]+ ms run delay, [0-9]+ ms host steal' \
              "$vlog" 2>/dev/null | tail -1)
_delay=$(sed -E 's/^vCPU starvation: ([0-9]+) ms.*/\1/' <<<"$_starve")
if [[ "$_delay" =~ ^[0-9]+$ ]]; then
    _wit="the guest's QEMU waited $_delay ms for a CPU"
    if (( verdict != 0 && verdict != 3 && _delay > STARVE_MAX_MS )) \
       && timing_only "$vlog" "$serial"; then
        verdict=3
        detail="UNJUDGED -- timing bounds missed while $_wit (> $STARVE_MAX_MS ms): load, not a verdict; $ran"
    else
        detail="$detail; $ran; $_wit"
    fi
else
    detail="$detail; $ran; vCPU starvation not measured"
fi

# Beside the run's files too: under ctest a SKIPPED gate.qloop's output
# reaches only Testing/Temporary/LastTest.log, which every later ctest
# invocation on the tree rewrites (a --show-only listing included).
echo "qloop-verdict: $detail" > "$dir/qloop-verdict.txt"
if (( own )); then
    if (( verdict == 0 )); then rm -rf "${dir:?}"; else echo "qloop-verdict: logs kept in $dir"; fi
fi
echo "qloop-verdict: $detail"
exit "$verdict"
