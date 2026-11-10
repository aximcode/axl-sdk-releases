#!/bin/bash
# unit-ratchet.sh -- the unit-test RATCHET over per-binary counts.
#
# test-axl.sh runs every unit binary in one boot and holds the total
# assertion count (passes + declared skips) to test/integration/.last-pass-count:
# the count may not fall, and a GREEN run that raised it writes the new value.
# Under CTest each binary is its own test (unit.<App>, slice 5a step 4,
# docs/AXL-Build-System-Design.md §10.8) that writes its count to
# TEST_COUNT_FILE after its verdict -- "<count> green|red", or from a run
# that knows them "<count> green|red <guest_ms|-> pooled|booted
# <contention|->" (common-test.sh) -- and this is the unit.counts fixture's
# cleanup, which sums them.
#
# - A WHOLE run only. A binary with no count did not run this time (`ctest -R`
#   one binary), or failed before reporting (its own test is red): a partial
#   sum is not evidence either way, so it exits 77 (CTest's SKIP), says so,
#   and never writes.
# - The counts are CONSUMED once read. CTest runs a cleanup even when the
#   fixture's setup (the reset) was skipped (-FS), and the counts left by an
#   earlier run would otherwise be summed with this run's.
# - Every count is judged, red or green; the baseline is RAISED only when every
#   binary was green -- as test-axl.sh writes only on a green, leak-free,
#   assert-clean run. CTest runs this cleanup whatever its tests did.
# - An unreadable count or baseline is a FAILURE, named: a check that cannot
#   read its bar has not passed it.
#
# Usage: lib/unit-ratchet.sh COUNTS_DIR WRITE APP...
#   WRITE  1: a whole green run above the baseline raises it (the x64 tree)
#          0: judge only (the aa64 tree: one baseline, which x64 owns)
# TEST_BASELINE_FILE overrides the baseline (as for test-axl.sh).
# AXL_UNIT_NO_WRITE=1 judges but never writes (verify.sh --apps).
set -uo pipefail

dir="${1:?usage: unit-ratchet.sh COUNTS_DIR WRITE APP...}"
write="${2:?usage: unit-ratchet.sh COUNTS_DIR WRITE APP...}"
shift 2
(( $# > 0 )) || { echo "unit-ratchet.sh: no unit binaries named" >&2; exit 2; }
baseline="${TEST_BASELINE_FILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.last-pass-count}"
# shellcheck source=../../../scripts/axl-common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/scripts/axl-common.sh"
# A declared budget scale is refused before anything is judged, as the
# one-boot run refuses it (test_check_budget_scale).
# Empty is undeclared, as the one-boot wrapper's ${AXL_UNIT_BUDGET_SCALE:-1}.
if [[ -n "${AXL_UNIT_BUDGET_SCALE:-}" ]] && ! budget_scale_ok "$AXL_UNIT_BUDGET_SCALE"; then
    echo "FAIL: AXL_UNIT_BUDGET_SCALE='$AXL_UNIT_BUDGET_SCALE' is not a decimal between 1 and 3"
    exit 2
fi

declare -A count=()
total=0 have=0 red=0 n=$# booted=0 unmeasured=0 uncontended=0
missing=()
ptimes=""   # "ms factor" per pooled, measured binary, for the budget
for app in "$@"; do
    f="$dir/$app"
    if [[ ! -f "$f" ]]; then
        missing+=("$app")
        continue
    fi
    line=$(head -1 "$f")
    rm -f "$f"   # consumed: see the header
    if [[ ! "$line" =~ ^([0-9]+)\ (green|red)(\ ([0-9]+|-)\ (pooled|booted)\ ([0-9]+(\.[0-9]+)?|-))?$ ]]; then
        echo "FAIL: $app wrote no count it could read ('$line')"
        exit 1
    fi
    count[$app]=${BASH_REMATCH[1]}
    [[ "${BASH_REMATCH[2]}" == red ]] && red=$((red + 1))
    if [[ "${BASH_REMATCH[5]}" != pooled ]]; then
        booted=$((booted + 1))
    elif [[ "${BASH_REMATCH[4]}" == - ]]; then
        unmeasured=$((unmeasured + 1))
    elif [[ "${BASH_REMATCH[6]}" == - ]]; then
        uncontended=$((uncontended + 1))
    else
        ptimes+="${BASH_REMATCH[4]} ${BASH_REMATCH[6]}"$'\n'
    fi
    total=$((total + count[$app])); have=$((have + 1))
done

# One machine-readable line of the per-binary counts it read, in every
# outcome below and always LAST: verify.sh compares the two arches binary by
# binary, so it can name the one that diverged -- under --apps too.
counts_line() {
    local a out="unit-counts:"
    for a in "$@"; do [[ -n "${count[$a]+set}" ]] && out+=" $a=${count[$a]}"; done
    printf '%s\n' "$out"
}

if (( ${#missing[@]} > 0 )); then
    echo "PARTIAL: $have of $n unit binaries left a count this run (the others did not run, or failed before reporting); the ratchet judges only a whole run"
    echo "  no count: ${missing[*]}"
    counts_line "$@"
    exit 77
fi

expected=""
if [[ -f "$baseline" ]]; then
    expected=$(head -1 "$baseline")
    if [[ ! "$expected" =~ ^[0-9]+$ ]]; then
        echo "FAIL: the baseline $baseline holds no count ('$expected')"
        exit 1
    fi
    if (( total < expected )); then
        echo "FAIL: expected at least $expected unit assertions but only $total ran ($n binaries)"
        echo "  baseline: $baseline"
        for app in "$@"; do printf '  %-24s %s\n' "$app" "${count[$app]}"; done
        counts_line "$@"
        exit 1
    fi
fi
echo "unit ratchet: $total assertions across $n binaries (baseline ${expected:-none}; $red red)"
raise=0
if [[ "$write" == 1 && $red -eq 0 ]]; then
    if [[ -z "$expected" ]] || (( total > expected )); then raise=1; fi
fi
# AXL_UNIT_NO_WRITE=1: a scoped run (verify.sh --apps) never writes the
# baseline, even when it names every binary and is whole -- it says it did
# not, so it must not.
if (( raise )) && [[ "${AXL_UNIT_NO_WRITE:-0}" == 1 ]]; then
    echo "unit ratchet: baseline NOT raised to $total (AXL_UNIT_NO_WRITE=1: a scoped run never writes it)"
    raise=0
fi
# THE WALL-CLOCK BUDGET, the one-boot run's (test-axl.sh --budget): over
# POOLED per-binary runs the binaries' own run times are the suite's work, so
# they are judged against the same number, each scaled as the one boot is --
# by the larger of the host contention its own run measured and
# AXL_UNIT_BUDGET_SCALE (what verify.sh declares it runs beside). The budget
# printed is 110 s x the factor those make together, and the verdict is
# judged on the printed figures, so it can never read "120 s over a 147 s
# budget" -- in WHOLE seconds, as the one-boot run fails at budget + 1 s.
# Not judged, and said: the judge-only tree (WRITE 0, aa64), whose one-boot
# run skips its budget too; a run no front end declared a scale for (a bare
# `ctest --preset x64-suite` runs the units beside dozens of integration
# guests, a contention a run-queue factor cannot see all of -- verify.sh
# always declares one); a booted binary (its time includes its own boot); an
# unmeasured one (no timestamps: never a 0 s pass); and one whose host
# contention was not measured ("-": never an idle 1.00).
# The verdict is folded into the exit AFTER the baseline write: a slow run is
# not a short one -- every assertion ran and passed -- as common-test.sh's
# one-boot ratchet writes regardless of its budget.
budget_rc=0
budget_base="${TEST_MAX_DURATION:-}"
if [[ -z "$budget_base" ]] && ! budget_base=$("$(dirname "${BASH_SOURCE[0]}")/../test-axl.sh" --budget); then
    echo "FAIL: unit budget: test-axl.sh --budget could not say the budget"
    counts_line "$@"
    exit 1
fi
if [[ "$write" != 1 ]]; then
    echo "unit budget: not judged -- a judge-only tree (WRITE 0), as its one-boot run skips the budget too"
elif [[ -z "${AXL_UNIT_BUDGET_SCALE:-}" ]]; then
    echo "unit budget: not judged -- no front end declared what the run shared the box with (AXL_UNIT_BUDGET_SCALE unset)"
elif (( booted > 0 )); then
    echo "unit budget: not judged -- $booted of $n binaries ran booted (its guest time includes its own boot)"
elif (( unmeasured > 0 )); then
    echo "unit budget: not judged -- $unmeasured of $n binaries' guest time was not measured (a run log with no timestamps)"
elif (( uncontended > 0 )); then
    echo "unit budget: not judged -- $uncontended of $n binaries' host contention was not measured"
else
    read -r used_s factor budget_s over < <(awk -v b="$budget_base" \
        -v s="$AXL_UNIT_BUDGET_SCALE" '
        # Summed in whole ms: float seconds made 111.000 s read 110.999 and pass.
        NF == 2 { e = ($2 > s) ? $2 : s; ms += $1; j += $1 / 1000 / e }
        END {
            f = (j > 0) ? sprintf("%.2f", ms / 1000 / j) : sprintf("%.2f", s)
            bud = int(b * f + 0.5); us = sprintf("%d.%d", int(ms / 1000), int((ms % 1000) / 100))
            print us, f, bud, (int(ms / 1000) > bud) ? 1 : 0
        }' <<< "$ptimes")
    if (( over )); then
        echo "FAIL: unit budget: $used_s s of pooled guest time across $n binaries is over the $budget_s s budget"
        budget_rc=1
    elif [[ "$factor" == 1.00 ]]; then
        echo "unit budget: $used_s s of pooled guest time across $n binaries (budget $budget_s s)"
    else
        echo "unit budget: $used_s s of pooled guest time across $n binaries (budget $budget_s s: $budget_base s x $factor)"
    fi
fi
if (( raise )); then
    # Atomic: an aa64 ratchet reading it mid-write would see an empty bar.
    printf '%s\n' "$total" > "$baseline.$$.tmp" && mv -f "$baseline.$$.tmp" "$baseline"
    echo "unit ratchet: baseline raised to $total"
fi
counts_line "$@"
exit "$budget_rc"
