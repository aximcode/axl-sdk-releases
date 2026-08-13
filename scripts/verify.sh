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
#   AXL_VERIFY_KEEP=1 ./scripts/verify.sh   # keep the per-job logs on success
#
# Exit non-zero if any job fails or the arches disagree.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$(dirname "$SCRIPT_DIR")" || exit 1

WANT_DOCS=1
for a in "$@"; do
    case "$a" in
        --no-docs) WANT_DOCS=0 ;;
        -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) echo "verify.sh: unknown option $a" >&2; exit 2 ;;
    esac
done

OUT=$(mktemp -d -t axl-verify.XXXXXX)
JOBS=(x64 aa64 lint make)
[[ $WANT_DOCS -eq 1 ]] && JOBS+=(docs)

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
mapfile -t MAKE_CHECKS < <(make -s print-lint-gates | tr ' ' '\n' | grep -v '^$')
if (( ${#MAKE_CHECKS[@]} < 5 )); then
    echo "verify.sh: refusing to run -- 'make print-lint-gates' named only" \
         "${#MAKE_CHECKS[@]} gate(s). A gate runner that runs nothing reports" \
         "ALL GREEN forever." >&2
    exit 2
fi

run() { local name="$1"; shift; ( "$@" ) >"$OUT/$name.log" 2>&1; echo $? >"$OUT/$name.rc"; }

run x64  env timeout 900 ./test/integration/test-axl.sh &
run aa64 env TEST_SKIP_RATCHET=1 timeout 900 ./test/integration/test-axl.sh --arch AARCH64 &
run lint ./scripts/lint.sh &
run make make "${MAKE_CHECKS[@]}" &
[[ $WANT_DOCS -eq 1 ]] && run docs ./scripts/build-docs.sh &
wait

fail=0
printf '%-6s %-4s %s\n' JOB RC DETAIL
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
    esac
    printf '%-6s %-4s %s\n' "$j" "$rc" "${detail:-see $OUT/$j.log}"
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
x=$(_arch_total "$OUT/x64.log")  || x=""
a=$(_arch_total "$OUT/aa64.log") || a=""
if [[ -n "$x" && "$x" == "$a" ]]; then
    echo "cross-arch: both $x  OK"
else
    echo "cross-arch: MISMATCH x64=${x:-?} aa64=${a:-?}"
    fail=1
fi

if [[ $fail -eq 0 && "${AXL_VERIFY_KEEP:-0}" != "1" ]]; then
    rm -rf "$OUT"
    echo "ALL GREEN"
else
    echo "logs: $OUT"
    [[ $fail -eq 0 ]] && echo "ALL GREEN" || echo "SOMETHING FAILED"
fi
exit $fail
