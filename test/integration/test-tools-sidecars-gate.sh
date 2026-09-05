#!/bin/bash
# test-meta: arch=none needs= est=2 local-only=0
# test-tools-sidecars-gate.sh — the sidecar gate must be able to FAIL.
#
# WHY THIS EXISTS. share/ holds the JSON5 name databases the shipped tools
# auto-discover beside their own .efi. Two places staged them and each kept its
# own hand-maintained list; the workflow's said `pci-ids.json5 pci-class.json5`,
# written when PCI was the only tool with a database and never revisited when
# lsusb and memspd grew theirs. Every published axl-sdk-uefi-tools-* up to
# v4.6.0 therefore carries lsusb.efi and memspd.efi and neither of their
# databases.
#
# THE FIRST VERSION OF THE GATE COULD NOT FAIL, and that is the real subject
# here. It matched `share/*.json5` as a substring of the whole workflow step --
# and the comment explaining WHY a glob is used contains that literal string.
# So the gate reported `clean -- staged by glob` against a staging line
# reverted to a single file. A detector satisfied by its own rationale reads
# exactly like protection. Case 2 below is that precise scenario: the comment
# is left in place and only the code is reverted.
#
# Host-only: no QEMU, no build. Operates on a COPY -- the suite runs a parallel
# pool and this test's subject is a scanner over the worktree, so it must never
# mutate a tracked file.
#
# Usage: ./test/integration/test-tools-sidecars-gate.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
GATE_REL="scripts/check-tools-sidecars.py"

WORK="$(mktemp -d -t axl-sidecargate.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# A minimal tree holding exactly what the gate reads. Copied rather than
# cloned so this runs against the WORKING tree, uncommitted changes included --
# a gate is most worth testing before it is committed.
build_tree() {
    local dst="$1"
    mkdir -p "$dst/scripts" "$dst/.github/workflows" "$dst/share"
    cp "$PROJECT_DIR/$GATE_REL"                        "$dst/scripts/"
    cp "$PROJECT_DIR/scripts/install.sh"               "$dst/scripts/"
    cp "$PROJECT_DIR/scripts/make-uefi-tools-readme.py" "$dst/scripts/"
    cp "$PROJECT_DIR/.github/workflows/release.yml"    "$dst/.github/workflows/"
    cp "$PROJECT_DIR"/share/*.json5                    "$dst/share/"
}

# Run the gate inside a tree. Prints its combined output; returns its status.
run_gate() { python3 "$1/$GATE_REL" 2>&1; }

echo "=== 1. baseline: the gate passes on an unmodified tree ==="
BASE="$WORK/base"; build_tree "$BASE"
if out="$(run_gate "$BASE")"; then
    pass "clean tree passes"
else
    fail "clean tree FAILED -- every negative below is meaningless"
    sed 's/^/      /' <<<"$out" | head -6
fi

echo
echo "=== 2. the regression: a glob in a COMMENT must not satisfy it ==="
T2="$WORK/comment"; build_tree "$T2"
# Revert ONLY the staging code. The comment above it still contains the
# literal `share/*.json5`, which is what fooled the first version.
sed -i 's|SIDECARS=(share/\*\.json5)|SIDECARS=(share/pci-ids.json5)|' \
    "$T2/.github/workflows/release.yml"
if grep -q 'SIDECARS=(share/pci-ids.json5)' "$T2/.github/workflows/release.yml"; then
    pass "fixture applied: workflow staging reverted to a one-file list"
else
    fail "fixture did NOT apply -- the sed matched nothing, so case 2 proves nothing"
fi
# The trap must still be present, or this tests the wrong thing.
if grep -q '^\s*#.*share/\*\.json5' "$T2/.github/workflows/release.yml"; then
    pass "fixture intact: a comment still contains the literal share/*.json5"
else
    fail "no commented glob left in the fixture -- case 2 is not the regression"
fi
if out="$(run_gate "$T2")"; then
    fail "a one-file list passed because a COMMENT contained the glob"
    sed 's/^/      /' <<<"$out" | head -4
elif grep -q 'release.yml' <<<"$out"; then
    pass "reverting the workflow to a list FAILS the gate, naming release.yml"
else
    pass "reverting the workflow to a list FAILS the gate"
    echo "      (note: failure did not name release.yml)"
fi

echo
echo "=== 3. the second staging path is covered too ==="
T3="$WORK/installer"; build_tree "$T3"
sed -i 's|_sidecars=("$LIBAXL_DIR"/share/\*\.json5)|_sidecars=("$LIBAXL_DIR"/share/pci-ids.json5)|' \
    "$T3/scripts/install.sh"
if grep -q '_sidecars=("$LIBAXL_DIR"/share/pci-ids.json5)' "$T3/scripts/install.sh"; then
    pass "fixture applied: installer staging reverted to a one-file list"
else
    fail "fixture did NOT apply -- case 3 proves nothing"
fi
if out="$(run_gate "$T3")"; then
    fail "the installer reverting to a list was reported CLEAN"
elif grep -q 'install.sh' <<<"$out"; then
    pass "reverting the installer to a list FAILS the gate, naming install.sh"
else
    fail "gate failed but never named install.sh"
    sed 's/^/      /' <<<"$out" | head -4
fi

echo
echo "=== 4. a staged-but-undocumented sidecar fails LOCALLY, not at tag time ==="
T4="$WORK/undocumented"; build_tree "$T4"
printf '{ vendors: [] }\n' > "$T4/share/zz-new-ids.json5"
if out="$(run_gate "$T4")"; then
    fail "a sidecar with no SIDECAR_DOCS entry was reported CLEAN"
elif grep -q 'zz-new-ids.json5' <<<"$out"; then
    pass "an undocumented sidecar FAILS the gate, naming the file"
else
    fail "gate failed but never named zz-new-ids.json5"
    sed 's/^/      /' <<<"$out" | head -4
fi

echo
echo "=== 5. an empty share/ is 'could not see', not 'nothing to report' ==="
T5="$WORK/empty"; build_tree "$T5"
rm -f "$T5"/share/*.json5
if out="$(run_gate "$T5")"; then
    fail "an empty share/ was reported CLEAN -- a gate with nothing to check"
else
    pass "an empty share/ FAILS rather than passing vacuously"
fi

echo
echo "=== 6. a renamed workflow step is reported, not silently skipped ==="
T6="$WORK/renamed"; build_tree "$T6"
sed -i 's|- name: Package tools tarball|- name: Package tools tarball RENAMED|' \
    "$T6/.github/workflows/release.yml"
if grep -q 'Package tools tarball RENAMED' "$T6/.github/workflows/release.yml"; then
    pass "fixture applied: the workflow step is renamed"
else
    fail "fixture did NOT apply -- case 6 proves nothing"
fi
if out="$(run_gate "$T6")"; then
    fail "the gate could not find its step and reported CLEAN anyway"
elif grep -qi 'renamed step\|no .* step' <<<"$out"; then
    pass "a step it cannot find FAILS and says so, rather than reporting clean"
else
    fail "gate failed but not for the missing-step reason"
    sed 's/^/      /' <<<"$out" | head -4
fi

echo
echo "tools-sidecars-gate: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
