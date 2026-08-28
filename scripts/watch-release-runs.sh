#!/bin/bash
# watch-release-runs.sh — poll GitHub Actions for the release tag's
# CI / Release / Docs workflows via GraphQL (separate quota pool;
# ~60 calls/hour vs ~1,800 for three parallel `gh run watch`).
#
# Usage:
#   scripts/watch-release-runs.sh                  # watch HEAD
#   scripts/watch-release-runs.sh v0.13.1          # watch tag
#   scripts/watch-release-runs.sh <sha>            # watch arbitrary sha
#
# Exits rc=0 if all workflows reach SUCCESS,
#   rc=1 if any workflow finishes non-SUCCESS,
#   rc=2 on input/usage errors.
#
# Why GraphQL: per docs/RELEASING.md, REST polling (`gh run watch`)
# generates ~1 req/3s/watcher and three parallel watchers will burn
# the 5,000-req/hr REST quota on a slow run. GraphQL has its own
# 5,000-points/hr quota that REST polling doesn't touch, and a
# single status query costs ~1 point.

set -euo pipefail

OWNER=aximcode
REPO=axl-sdk
INTERVAL=${POLL_INTERVAL_S:-60}      # seconds between polls
TARGET=${1:-HEAD}

if ! command -v gh >/dev/null 2>&1 || ! command -v jq >/dev/null 2>&1; then
    echo "ERROR: gh and jq must be installed" >&2
    exit 2
fi

# Dereference to the commit sha. `git rev-parse v0.14.0` on an
# annotated tag returns the *tag*'s sha (a Tag object); the
# GraphQL `... on Commit` fragment below would silently miss and
# checkSuites.nodes would come back null. The `^{commit}` peel
# resolves to the underlying commit for both annotated and
# lightweight tags as well as branches and bare shas.
SHA=$(git rev-parse "${TARGET}^{commit}" 2>/dev/null) || {
    echo "ERROR: cannot resolve '$TARGET' to a commit sha" >&2
    exit 2
}

# Which workflows must appear AND succeed before we render a verdict.
# EVERY `v*` tag triggers Release + Docs -- docs.yml's trigger is
# `push: tags: ['v*']` with no version filter. (This said "a MAJOR tag push
# triggers Release + Docs, a minor/patch only Release", which meant a minor
# release never WAITED for Docs at all.) CI is NOT triggered by tags — it is dispatched + watched on main
# BEFORE tagging (see docs/RELEASING.md §4b). Their check suites are created at
# slightly different times, so an early snapshot can contain only the
# fast/finished one (e.g. Docs) while Release is still spinning up. If we judged
# on that snapshot we would declare PASS before Release even registered —
# exactly what burned the v1.2.0 cut. So we wait until every EXPECTED workflow
# has a check suite AND none are still running.
#
# The tag default below assumes a MAJOR tag (Release + Docs). For a minor/patch
# tag (Release only) or any other shape, pass EXPECT_WORKFLOWS — cut-release.sh
# always does. Non-tag targets (a branch HEAD / bare sha) only see CI if you
# dispatched it manually.
if [[ -n "${EXPECT_WORKFLOWS:-}" ]]; then
    read -ra EXPECTED <<< "$EXPECT_WORKFLOWS"
elif git rev-parse --verify --quiet "refs/tags/$TARGET" >/dev/null 2>&1; then
    EXPECTED=(Release Docs)
else
    EXPECTED=(CI Docs)
fi

# Safety ceiling so a workflow that never triggers can't hang the cut.
MAX_POLLS=${MAX_POLLS:-90}            # 90 * 60s = 90 min worst case

echo "Watching $OWNER/$REPO @ ${SHA:0:12} (poll every ${INTERVAL}s, GraphQL)"
echo "Expecting: ${EXPECTED[*]}"
echo

terminal=0
poll=0
while [[ $terminal -eq 0 ]]; do
    res=$(gh api graphql -f query="
    { repository(owner:\"$OWNER\",name:\"$REPO\") {
        object(expression:\"$SHA\") { ... on Commit {
          checkSuites(first:10) { nodes {
            workflowRun { workflow { name } }
            status conclusion
          } }
    } } } }" 2>/dev/null \
    | jq -r '.data.repository.object.checkSuites.nodes[] |
             select(.workflowRun != null) |
             "\(.workflowRun.workflow.name)\t\(.status)\t\(.conclusion // "-")"' \
    | sort)

    if [[ -z "$res" ]]; then
        echo "[$(date +%H:%M:%S)] no workflows found yet"
    else
        echo "[$(date +%H:%M:%S)]"
        printf '  %s\n' "$res" | column -ts $'\t'
    fi

    # Expected workflows that haven't registered a check suite yet.
    present=$(echo "$res" | awk -F'\t' 'NF {print $1}')
    missing=()
    for w in "${EXPECTED[@]}"; do
        grep -qxF "$w" <<< "$present" || missing+=("$w")
    done
    [[ ${#missing[@]} -gt 0 ]] && echo "  waiting for: ${missing[*]}"
    echo

    # ONLY THE EXPECTED WORKFLOWS GATE. Others on this SHA are reported and
    # ignored, and the distinction is worth 69% of a cut: measured on v4.3.1,
    # Release finished at 4m02s and Docs at 5m02s, then this loop sat on CI for
    # another 11m05s of a 16m07s run.
    #
    # CI is not started by the tag -- it is started by the `git push origin
    # main` cut-release.sh performs two steps earlier -- and it re-runs the same
    # integration suite the LOCAL uncached gate already certified on that exact
    # commit. It also finishes long after the tag is pushed and the release is
    # published, so blocking on it cannot prevent anything; it only delays the
    # human and, when red, mislabels a successful release (v4.3.0 published all
    # 8 assets and reported RELEASE_VERDICT: FAIL for a CI container defect).
    gating=$(printf '%s\n' "$res" | awk -F'\t' -v ws="${EXPECTED[*]}" '
        BEGIN { n = split(ws, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
        NF && ($1 in want)')
    other=$(printf '%s\n' "$res" | awk -F'\t' -v ws="${EXPECTED[*]}" '
        BEGIN { n = split(ws, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
        NF && !($1 in want)')
    [[ -n "$other" ]] && printf '  (not gating this cut: %s)\n' \
        "$(printf '%s\n' "$other" | awk -F'\t' '{printf "%s=%s ", $1, ($3=="-"?$2:$3)}')"

    running=false
    [[ -n "$gating" ]] && grep -qE "QUEUED|IN_PROGRESS" <<<"$gating" && running=true

    if ! $running && [[ ${#missing[@]} -eq 0 ]]; then
        terminal=1
        break
    fi

    poll=$((poll + 1))
    if [[ $poll -ge $MAX_POLLS ]]; then
        echo "FAIL — timed out after $poll polls; still pending/missing."
        [[ ${#missing[@]} -gt 0 ]] && echo "       never appeared: ${missing[*]}"
        echo "RELEASE_VERDICT: FAIL"
        exit 1
    fi
    sleep "$INTERVAL"
done

# Final verdict — exit non-zero if any conclusion isn't SUCCESS.
# Also print a machine-greppable verdict line: when the script is
# piped through `tail` / `head` / etc. the pipe swallows the exit
# code, so automation should grep for `RELEASE_VERDICT:` rather
# than rely on $?. Use `set -o pipefail` in the caller's shell if
# you want the original exit to propagate through pipelines.
echo
if printf '%s\n' "$gating" | awk -F'\t' 'NF {print $3}' | grep -qvE "^SUCCESS$"; then
    echo "FAIL — a workflow this tag is responsible for did not succeed."
    echo "RELEASE_VERDICT: FAIL"
    exit 1
fi
if [[ -n "$other" ]]; then
    echo "Not gating this cut (reported only, does not gate the release):"
    printf '%s\n' "$other" | column -ts $'\t' | sed 's/^/  /'
    echo "  These are not started by the tag. A red one is worth reading —"
    echo "  'gh run list' — but it cannot un-publish a release that is already out."
fi
echo "Expected workflows green: ${EXPECTED[*]}."
echo "RELEASE_VERDICT: PASS"
exit 0
