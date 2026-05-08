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

echo "Watching $OWNER/$REPO @ ${SHA:0:12} (poll every ${INTERVAL}s, GraphQL)"
echo

terminal=0
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
    echo

    if ! echo "$res" | grep -qE "QUEUED|IN_PROGRESS"; then
        terminal=1
        break
    fi
    sleep "$INTERVAL"
done

# Final verdict — exit non-zero if any conclusion isn't SUCCESS.
echo
if echo "$res" | awk -F'\t' '{print $3}' | grep -qvE "^SUCCESS$"; then
    echo "FAIL — at least one workflow did not succeed."
    exit 1
fi
echo "All workflows green."
exit 0
