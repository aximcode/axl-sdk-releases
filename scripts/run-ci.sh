#!/bin/bash
# run-ci.sh — manually trigger the CI suite (build + QEMU integration + lint) on
# demand and watch it. CI does NOT run on code pushes anymore (only on vX.0.0
# tags or this dispatch) — the authoritative gate is the LOCAL suite, see
# docs/RELEASING.md. Use this when you specifically want a fresh-OS CI pass.
#
# Usage:
#   scripts/run-ci.sh                 # run CI on origin/main
#   scripts/run-ci.sh <branch|tag>    # run CI on a specific ref
#   scripts/run-ci.sh --docs          # also (or instead) trigger the Docs deploy
set -euo pipefail

REF="main"
WHICH="ci.yml"
for arg in "$@"; do
    case "$arg" in
        --docs)  WHICH="docs.yml" ;;
        -*)      echo "usage: run-ci.sh [ref] [--docs]" >&2; exit 2 ;;
        *)       REF="$arg" ;;
    esac
done

command -v gh >/dev/null || { echo "gh CLI required" >&2; exit 1; }

echo ">> dispatching $WHICH on $REF"
gh workflow run "$WHICH" --ref "$REF"
sleep 8   # let the run register

rid="$(gh run list --workflow "$WHICH" --limit 1 --json databaseId --jq '.[0].databaseId')"
echo ">> watching run $rid  (Ctrl-C to stop watching; the run keeps going)"
gh run watch "$rid" --exit-status
