#!/bin/bash
# verify.sh — the pre-commit gate set, run concurrently.
#
# Serially these are ~4 minutes: X64 suite ~50s, AARCH64 ~50s, lint ~2m (the
# clang -Wall -Wextra pass, clang-tidy over src/, clang-tidy over test/unit/),
# docs ~30s, make checks ~15s. They are independent, so this runs them at once
# and prints one table. ~2 minutes wall, and clang-tidy is the long pole.
#
# Nothing collides on disk: the heavy jobs use separate build prefixes
# (out/native-x64, out/native-aa64, out/native-x64-lint) and build-docs.sh
# builds no objects at all.
#
# ONE piece of shared writable state: test/integration/.last-pass-count, which
# BOTH arch runs write on success. Rather than race it, the AARCH64 run gets
# TEST_SKIP_RATCHET=1 and its count is asserted EQUAL to X64's afterwards.
# That is stronger than the serial habit it replaces -- the ratchet still gates
# through X64, and the cross-arch equality, which nothing checked before
# (it was compared by eye), is now an assertion that fails the run.
#
# Usage:
#   ./scripts/verify.sh              # everything
#   ./scripts/verify.sh --no-docs    # skip the Sphinx build (slowest to rebuild)
#   ./scripts/verify.sh --only=docs  # run ONLY these jobs (comma-separated)
#   AXL_VERIFY_KEEP=1 ./scripts/verify.sh   # keep the per-job logs on success
#
# --only exists because the jobs are not equally able to SEE a given change.
# A markdown-only edit is invisible to x64/aa64/lint/make, so running them
# costs two arch builds plus a full AXL_CPP=1 lint build for no information --
# and build-docs.sh runs INSIDE this script, so calling it separately first
# pays for Sphinx twice. `--only=docs` is the whole gate for a prose change.
#
# A filtered run NEVER prints a bare "ALL GREEN": it names what did not run.
# Otherwise a --only run and a full run are indistinguishable in a log, which
# is the failure mode this tree keeps meeting -- a gate that cannot see the
# change reporting the same green as one that can.
#
# Exit non-zero if any job fails or the arches disagree.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$(dirname "$SCRIPT_DIR")" || exit 1

WANT_DOCS=1
ONLY=""
ALL_JOBS=(x64 aa64 lint make makeimg docs)
for a in "$@"; do
    case "$a" in
        --no-docs) WANT_DOCS=0 ;;
        --only=*)  ONLY="${a#--only=}" ;;
        -h|--help) sed -n '2,38p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "verify.sh: unknown option $a" >&2; exit 2 ;;
    esac
done

OUT=$(mktemp -d -t axl-verify.XXXXXX)
SKIPPED=()
if [[ -n "$ONLY" ]]; then
    IFS=',' read -r -a JOBS <<< "$ONLY"
    # A typo must FAIL, not quietly select nothing and report ALL GREEN.
    for j in "${JOBS[@]}"; do
        case " ${ALL_JOBS[*]} " in
            *" $j "*) ;;
            *) echo "verify.sh: --only names unknown job '$j'" \
                    "(known: ${ALL_JOBS[*]})" >&2; exit 2 ;;
        esac
    done
    for j in "${ALL_JOBS[@]}"; do
        case " ${JOBS[*]} " in *" $j "*) ;; *) SKIPPED+=("$j") ;; esac
    done
else
    JOBS=(x64 aa64 lint make makeimg)
    [[ $WANT_DOCS -eq 1 ]] && JOBS+=(docs) || SKIPPED+=(docs)
fi

want() {
    local j
    for j in "${JOBS[@]}"; do [[ "$j" == "$1" ]] && return 0; done
    return 1
}

# The pure-lint make gates: each reports "... clean" and builds no libaxl.a,
# so none of them can leave a binary stale against a changed ABI. That is what
# their NONCLEAN_GOALS exclusion turns on -- NOT "builds nothing", which stopped
# being true when check-fuzz-link joined (three host ASan binaries) and again
# when check-examples did (51 example objects under out/check-examples). Neither
# is linked into anything the firmware tree produces, so the exclusion holds.
#
# ASKED FOR, not listed. The Makefile owns the list as LINT_GATES and this reads
# it back, because the two copies drifted once and the consequence was not a
# stale build: an unexcluded gate WIPES $(BUILDDIR)/*.o and libaxl.a while the
# x64 and aa64 jobs below are mid-build. Deriving it makes that unrepresentable
# instead of merely documented. The DETAIL column stays count-agnostic (see the
# `make)` case), so a gate added in the Makefile needs no edit here at all.
MAKE_CHECKS=()
if want make; then
mapfile -t MAKE_CHECKS < <(make -s print-lint-gates | tr ' ' '\n' | grep -v '^$')
fi
if want make && (( ${#MAKE_CHECKS[@]} < 5 )); then
    echo "verify.sh: refusing to run -- 'make print-lint-gates' named only" \
         "${#MAKE_CHECKS[@]} gate(s). A gate runner that runs nothing reports" \
         "ALL GREEN forever." >&2
    exit 2
fi

# Each leg records its own wall time. Without this the table said only WHICH
# leg failed, never which one you were waiting for -- and "verify takes about
# ten minutes" is not a number you can act on. The TIME column turns the next
# optimisation into a measurement instead of a guess.
run() {
    local name="$1"; shift
    local t0=$SECONDS
    ( "$@" ) >"$OUT/$name.log" 2>&1
    echo $? >"$OUT/$name.rc"
    echo $(( SECONDS - t0 )) >"$OUT/$name.sec"
}

want x64  && run x64  env timeout 900 ./test/integration/test-axl.sh &
want aa64 && run aa64 env TEST_SKIP_RATCHET=1 timeout 900 ./test/integration/test-axl.sh --arch AARCH64 &
want lint && run lint ./scripts/lint.sh &
want make && run make make "${MAKE_CHECKS[@]}" &
want docs && run docs ./scripts/build-docs.sh &
wait

# THE ARTIFACT GATES RUN HERE, AFTER the wait, and the placement is the point.
# check-pe-stripped and check-log-linkage take $(PREFIX)/*.efi as prerequisites,
# so each runs the libaxl.a recipe -- which begins `rm -f $@` -- and the x64 job
# above builds into that same default PREFIX. Run beside it, they are two
# unsynchronised `make` processes racing one archive; nothing here locks.
#
# Deriving LINT_GATES from the Makefile made the two LISTS unable to drift.
# That is not the same property as every member being safe to run beside a
# build, which is what its comment claims -- so the Makefile now splits the
# two, and membership of LINT_GATES means what it says again.
#
# Serial and last is also the CHEAPEST place: the x64 job has just built every
# image these need, so both are a read of warm artifacts rather than a build.
if want makeimg; then
    mapfile -t ART_CHECKS < <(make -s print-lint-gates-artifact | tr ' ' '\n' | grep -v '^$')
    if (( ${#ART_CHECKS[@]} == 0 )); then
        echo "verify.sh: refusing to run -- 'make print-lint-gates-artifact'" \
             "named no gates. A gate runner that runs nothing reports ALL" \
             "GREEN forever." >&2
        exit 2
    fi
    run makeimg make "${ART_CHECKS[@]}"
fi

fail=0
printf '%-6s %-4s %6s %s\n' JOB RC TIME DETAIL
for j in "${JOBS[@]}"; do
    rc=$(cat "$OUT/$j.rc" 2>/dev/null || echo '?')
    case "$j" in
        x64|aa64) detail=$(grep -oE '[0-9]+ passed, [0-9]+ failed \([A-Z0-9]+\)' "$OUT/$j.log" | tail -1) ;;
        lint)     detail=$(grep -oE '^lint: clean.*' "$OUT/$j.log" | tail -1) ;;
        docs)     detail="$(grep -ciE 'warning:|error:' "$OUT/$j.log") warnings/errors" ;;
        # Count the "... clean" lines, with no denominator: gates are not 1:1
        # with lines (check-dogfood alone prints two), so any fixed total is
        # wrong the moment a gate is added OR grows a second line -- "7 of 6
        # clean" was the first thing this printed. RC is the real verdict; this
        # column is informational.
        make)     detail="$(grep -c ': clean' "$OUT/$j.log") gate lines clean" ;;
        makeimg)  detail="$(grep -c ': clean\|: OK' "$OUT/$j.log") image gate(s) clean (serial: they build)" ;;
    esac
    _sec=$(cat "$OUT/$j.sec" 2>/dev/null || echo '?')
    printf '%-6s %-4s %5ss %s\n' "$j" "$rc" "$_sec" "${detail:-see $OUT/$j.log}"
    [[ "$rc" == "0" ]] || fail=1
done

# Cross-arch equality. The totals must match exactly -- a divergence means a
# test is arch-gated without a balancing SKIP, which the per-arch ratchet
# cannot see because each arch only ever compares against its own history.
#
# The compared quantity is pass + DECLARED-SKIP assertions, not pass alone.
# Balancing is what test_skip_n does: a gated group declares the assertions the
# populated path would have run, so a balanced gate MOVES assertions out of
# pass and into skipped rather than dropping them. aa64 has no SMBIOS OEM
# strings and no nvstore, so it legitimately passes 8 fewer and declares 8 more
# than X64 -- both totalling 10192. Comparing pass alone called that a failure,
# which is backwards: it red-flagged the balanced case it was written to bless,
# and it had been red on worktree-json-flag-redesign for exactly that reason
# before anyone read the number.
#
# It still catches the defect it was written for. An arch gate with NO balancer
# declares nothing, so its assertions leave pass without arriving in skipped,
# the total drops on that arch alone, and this fails -- which is the whole
# point, since the per-arch ratchet only ever compares an arch to itself.
_arch_total() {                 # Results: N passed, ... SKIPPED (M assertions) (ARCH) in ...
    local line n_pass n_skip
    line=$(grep -aE '^Results:' "$1" | tail -1)
    n_pass=$(sed -nE 's/.*: ([0-9]+) passed.*/\1/p'                <<<"$line")
    n_skip=$(sed -nE 's/.*SKIPPED \(([0-9]+) assertions\).*/\1/p'  <<<"$line")
    [[ -n "$n_pass" ]] || return 1
    echo $(( n_pass + ${n_skip:-0} ))
}
# Only meaningful when BOTH arches ran; otherwise there is nothing to compare
# and reporting a mismatch would be an artifact of the filter.
if ! (want x64 && want aa64); then
    x=""; a=""
else
x=$(_arch_total "$OUT/x64.log")  || x=""
a=$(_arch_total "$OUT/aa64.log") || a=""
fi
if ! (want x64 && want aa64); then
    echo "cross-arch: not compared (both arches were not run)"
elif [[ -n "$x" && "$x" == "$a" ]]; then
    echo "cross-arch: both $x  OK"
else
    echo "cross-arch: MISMATCH x64=${x:-?} aa64=${a:-?}"
    fail=1
fi

# A filtered run must never be mistakable for a full one in a log. Naming what
# did NOT run is the whole safety property of --only.
green="ALL GREEN"
if (( ${#SKIPPED[@]} )); then
    green="ALL GREEN (partial run -- NOT RUN: ${SKIPPED[*]})"
fi

if [[ $fail -eq 0 && "${AXL_VERIFY_KEEP:-0}" != "1" ]]; then
    rm -rf "$OUT"
    echo "$green"
else
    echo "logs: $OUT"
    [[ $fail -eq 0 ]] && echo "$green" || echo "SOMETHING FAILED"
fi
exit $fail
