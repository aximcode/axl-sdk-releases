#!/bin/bash
# test-meta: arch=x64 needs= est=5 local-only=0
# test-release-gate.sh — the stamp that lets a release skip a redundant CI run.
#
# This gate decides whether a release is verified. A false POSITIVE tags code
# no suite ever ran, so every case below is about refusing:
#   - no stamp, malformed stamp, a stamp from a FAILED run
#   - a stamp for an unrelated commit
#   - a stamp whose tree has since changed in anything but the version files
# and exactly one about accepting: the release commit's parent, whose only
# diff is VERSION / axl-version.h / CHANGELOG.md (design §10.3).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [[ "$2" == "$3" ]]; then pass "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
export AXL_STAMP_FILE="$TMP/stamp"
# shellcheck source=lib/release-gate.sh
source "$SCRIPT_DIR/lib/release-gate.sh"

# A throwaway repo, so the assertions do not depend on this tree's history.
REPO="$TMP/repo"; mkdir -p "$REPO"; cd "$REPO"
git init -q .; git config user.email t@t; git config user.name t
mkdir -p include/axl
echo 4.1.0 > VERSION; echo hdr > include/axl/axl-version.h
echo log > CHANGELOG.md; echo src > src.c
git add -A >/dev/null; git commit -qm base
BASE=$(git rev-parse HEAD)

echo "=== release-gate ==="

release_gate_covers "$BASE" >/dev/null 2>&1
check "no stamp -> refuses" "$?" "1"

release_gate_write "$BASE" X64 159 0
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp on HEAD -> accepts" "$?" "0"

release_gate_write "$BASE" X64 158 1
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp from a FAILED run -> refuses" "$?" "1"

printf 'sha=\narch=X64\n' > "$AXL_STAMP_FILE"
release_gate_covers "$BASE" >/dev/null 2>&1
check "malformed stamp -> refuses" "$?" "1"

# The release commit: only the three version files change.
release_gate_write "$BASE" X64 159 0
echo 4.2.0 > VERSION; echo hdr2 > include/axl/axl-version.h; echo log2 > CHANGELOG.md
git add -A >/dev/null; git commit -qm "release: v4.2.0"
REL=$(git rev-parse HEAD)
release_gate_covers "$REL" >/dev/null 2>&1
check "release commit over a stamped parent -> accepts" "$?" "0"

# The case that must never pass: real code changed after the stamp.
echo changed > src.c; git add -A >/dev/null; git commit -qm "a real change"
release_gate_covers "$(git rev-parse HEAD)" >/dev/null 2>&1
check "source changed since the stamp -> refuses" "$?" "1"

# A stamp for a commit that is not an ancestor at all.
git checkout -q -b other "$BASE"; echo x > other.c
git add -A >/dev/null; git commit -qm other
release_gate_write "$(git rev-parse HEAD)" X64 159 0
release_gate_covers "$REL" >/dev/null 2>&1
check "stamp from an unrelated commit -> refuses" "$?" "1"

echo ""

# --- the release commit must skip ci.yml, and only ci.yml ------------------
#
# cut-release.sh pushes the release commit to main BEFORE tagging, and ci.yml
# triggers on push -- so a release ran the full integration suite twice on this
# box: once as the local pre-release gate, once from that push, for a commit
# whose content is a version string and a date. release.yml then builds and
# tests it a third time on the tag.
#
# The marker is spelled in two files that cannot import each other, which is
# the drift shape this tree keeps paying for. Hold them equal here.
MARK='[release-cut]'
CUT="$PROJECT_DIR/scripts/cut-release.sh"
CI="$PROJECT_DIR/.github/workflows/ci.yml"

grep -qF -- "$MARK" "$CUT"
check "cut-release.sh writes the skip marker" "$?" "0"

n_guard=$(grep -cF -- "!contains(github.event.head_commit.message, '$MARK')" "$CI")
# Job keys are the 2-space-indented mappings under `jobs:`, and nothing else --
# an anchored `^  name:` regex over the whole file also catches step keys and
# matrix entries, which is how this first read "3 of 5".
n_jobs=$(awk '/^jobs:/{inj=1;next} /^[^[:space:]]/{inj=0}
              inj && /^  [A-Za-z][A-Za-z0-9_-]*:[[:space:]]*$/{n++}
              END{print n+0}' "$CI")
[[ "$n_guard" -eq "$n_jobs" && "$n_guard" -gt 0 ]]
check "every ci.yml job is guarded by it ($n_guard of $n_jobs)" "$?" "0"

# NOT `[skip ci]`: GitHub honours that natively by inspecting the pushed HEAD
# commit, and the TAG push carries the same commit -- it would skip release.yml
# and docs.yml too and publish nothing at all.
#
# Comments stripped first. The comment in cut-release.sh explaining why we do
# NOT use `[skip ci]` contains the string, so scanning the raw file finds it
# and fails -- a check that cannot survive its own subject being discussed.
! sed 's/#.*//' "$CUT" | grep -qiE '\[(skip ci|ci skip|no ci|skip actions|actions skip)\]'
check "the marker is OURS, not a GitHub-native skip string" "$?" "0"

# ...and the publish workflows must carry no such guard, or a release would
# silently not publish.
for _wf in release docs; do
    ! grep -qF -- "$MARK" "$PROJECT_DIR/.github/workflows/$_wf.yml"
    check "$_wf.yml does NOT skip on the marker" "$?" "0"
done

echo "release-gate: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
