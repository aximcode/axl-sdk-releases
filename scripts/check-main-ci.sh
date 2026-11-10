#!/bin/bash
# check-main-ci.sh -- does origin/main's tip have a CI run at all?
#
# WHY THIS EXISTS. On 2026-09-05 a push to `main` produced NO workflow run.
# Not a skipped one -- a skipped run still exists and still lists, which is how
# a `[release-cut]` commit looks. There was no run object. `ci.yml` was active,
# its trigger is `push: branches: [main]` with no path filter, and the commit
# subject did not match the skip guard. The event simply did not fire, which is
# below anything this repo controls.
#
# THE PROBLEM IS NOT THE MISSED RUN, IT IS THE SILENCE. Everything downstream
# fails closed: the release gate refuses a commit with no green run of the
# jobs it names, so a missed event can only BLOCK a release, never let an
# unverified one through. But nothing goes red, nothing is unusual to look at,
# and the gap is discovered at cut time -- the worst moment, because that is
# when someone is waiting.
#
# WHAT IT REPORTS, and the states are deliberately distinct:
#
#   green     a completed CI run at the tip whose CHEAP jobs (gcc x64,
#             gcc aa64, clang-tidy) all succeeded
#   pending   the run that DECIDES the answer is queued or in progress
#   MISSING   no run object at all, or one that ran nothing
#
# THE CHEAP JOBS, NOT THE SUITE (AXL-CI-Release-Speed-Design.md §14). Since
# 2026-10-21 a push runs plan + the three cheap jobs, and the QEMU suite runs
# only on workflow_dispatch -- so a push run never contains it, and asking for
# it would report every healthy push NOT GREEN. Whether the suite covered a
# commit is the release gate's question (release_gate_ci_covers), not this
# one's.
#
# Exit status: 0 green or pending; 1 MISSING or NOT GREEN; 2 could not check
# (no gh, a gh that fails, a ref that does not resolve). An empty answer from
# a query that did not run is not a missing run.
#
# "MISSING" is the one worth acting on, and the action is one line:
#   gh workflow run ci.yml --ref main
#
# A TRANSIENT SEEN ON THE FIRST RUN OF THIS SCRIPT, recorded because it also
# affects the release gate. GitHub listed the run as completed/success while
# `gh run view --json jobs` still returned an EMPTY array, so
# the gate's helper saw no jobs and refused -- correctly, by its own rule
# that a run reporting no jobs cannot certify anything. A minute later the same
# query answered green. It fails CLOSED, so nothing unsafe follows from it; the
# cost is that a cut could be told "nothing covers this commit" and send
# someone to run the local suite for no reason. A single retry would separate
# "transiently empty" from "genuinely no jobs" -- not added here, because a
# retry that exists to make a check say yes needs more care than a tired
# evening affords.
#
# NOT A GATE, deliberately. It answers a question about GitHub's state, needs
# the network, and cannot be part of verify.sh or the gate.check-* set. Run
# it when a push seems to have vanished, before a release, or from a schedule
# if you decide you want one -- see docs/RELEASING.md.
#
# Usage: scripts/check-main-ci.sh [ref]        (default: origin/main)
set -uo pipefail

REF="${1:-origin/main}"
SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v gh >/dev/null 2>&1 || { echo "check-main-ci: gh is not on PATH" >&2; exit 2; }

# The REMOTE tip, not the local one: the question is whether what was PUSHED
# got a run, and a local HEAD that was never pushed would answer the wrong
# question in the reassuring direction.
if [[ "$REF" == origin/* ]]; then
    branch="${REF#origin/}"
    sha=$(git ls-remote origin "$branch" 2>/dev/null | awk '{print $1}')
else
    sha=$(git rev-parse --verify -q "$REF" 2>/dev/null)
fi
[[ -n "$sha" ]] || { echo "check-main-ci: cannot resolve $REF" >&2; exit 2; }

short="${sha:0:8}"

# A run that is queued or running is not missing. Asked separately from the
# green check, because "no run" and "not finished yet" want opposite responses
# and look identical if you only ask "is it green".
if ! state=$(gh run list --workflow=ci.yml --commit "$sha" --limit 10 \
                 --json status,conclusion \
                 --jq '.[] | "\(.status)/\(.conclusion // "-")"' 2>/dev/null); then
    echo "check-main-ci: could not check -- \`gh run list\` failed for $short" >&2
    exit 2
fi

if [[ -z "$state" ]]; then
    cat >&2 <<EOF
check-main-ci: MISSING -- no ci.yml run exists for $REF ($short)

  A SKIPPED run would still exist and still be listed; this is no run at all,
  which is what a dropped push event looks like. Nothing is unsafe -- the
  release gate refuses a commit it cannot see a green run for -- but nothing
  is verifying this tree either.

  Dispatch one:
      gh workflow run ci.yml --ref ${REF#origin/}
EOF
    exit 1
fi

# THE WALK, NOT THE TIP ALONE (review ruling R-E). A [release-cut] or
# docs-only tip's own run skips the cheap jobs by design, so asking the tip
# alone reported a healthy main NOT GREEN after every docs push and every
# release. The release gate's bounded walk answers it from the commit below,
# and "pending" means the run that DECIDES is still going -- not merely that
# some run somewhere is, which reported a red code run as "pending" while a
# docs push's run sat on top of it.
# shellcheck source=../test/integration/lib/release-gate.sh
source "$SCRIPT_DIR/../test/integration/lib/release-gate.sh"
# The walk reads history, so the commit has to be HERE. A fetch that has not
# happened is "could not check", not a red tree.
git cat-file -e "$sha^{commit}" 2>/dev/null || {
    echo "check-main-ci: could not check -- $short is not in this repository (git fetch)" >&2
    exit 2; }
cheap=$(_release_gate_join ', ' "${AXL_CI_CHEAP_JOBS[@]}")
why=$(_release_gate_walk_back "$sha" _release_gate_ci_cheap_green "cheap push CI ($cheap)")
rc=$?
case $rc in
    0)  echo "check-main-ci: green -- $why"
        exit 0 ;;
    3)  echo "check-main-ci: pending -- the run that decides $sha is still going"
        sed '1d' <<< "$why"
        exit 0 ;;
    2)  echo "check-main-ci: could not check -- $why" >&2
        exit 2 ;;
esac

cat >&2 <<EOF
check-main-ci: NOT GREEN -- no green push run of the cheap jobs covers $short
  runs seen at the tip: $(tr '\n' ' ' <<< "$state")
$(sed 's/^/  /' <<< "$why")

  Look before re-dispatching:
      gh run list --workflow=ci.yml --commit $sha
EOF
exit 1
