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
echo "release-gate: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
