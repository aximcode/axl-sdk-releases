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
# A tag push triggers CI + Release + Docs; their check suites are created
# at slightly different times, so an early snapshot can contain only the
# fast/finished ones (e.g. CI+Docs) while Release is still spinning up. If
# we judged on that snapshot we would declare PASS before Release even
# registered — exactly what burned the v1.2.0 cut. So we wait until every
# EXPECTED workflow has a check suite AND none are still running.
#
# Non-tag targets (a branch HEAD / bare sha) don't trigger Release, so we
# only expect CI + Docs there. Override either with EXPECT_WORKFLOWS.
if [[ -n "${EXPECT_WORKFLOWS:-}" ]]; then
    read -ra EXPECTED <<< "$EXPECT_WORKFLOWS"
elif git rev-parse --verify --quiet "refs/tags/$TARGET" >/dev/null 2>&1; then
    EXPECTED=(CI Release Docs)
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

    running=false
    echo "$res" | grep -qE "QUEUED|IN_PROGRESS" && running=true

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
if echo "$res" | awk -F'\t' '{print $3}' | grep -qvE "^SUCCESS$"; then
    echo "FAIL — at least one workflow did not succeed."
    echo "RELEASE_VERDICT: FAIL"
    exit 1
fi
echo "All workflows green."
echo "RELEASE_VERDICT: PASS"
exit 0
