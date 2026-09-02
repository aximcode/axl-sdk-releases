#!/bin/bash
# test-meta: arch=none needs= est=8 local-only=0
# test-source-snapshot.sh — what the public repo is about to receive.
#
# WHY THIS EXISTS. Each release pushes a squashed source snapshot to the PUBLIC
# `aximcode/axl-sdk-releases`. The selection rule was "everything tracked, minus
# .github/" -- one exclusion, for a mechanical reason rather than an editorial
# one -- so every tracked document was public, including internal working
# notes, and nobody had decided that (AXL-Distribution-Design.md §15).
#
# §15.2 names two properties such a gate must have, both learned the hard way
# in this tree, and both are asserted below:
#
#   - IT MUST BE SHOWN TO FAIL. A gate that has never caught anything is a gate
#     nobody has proven can see. Each forbidden class is PLANTED in a scratch
#     snapshot here and the check must fire on it.
#   - IT MUST DISTINGUISH "found nothing" from "could not run." A grep over a
#     path that does not exist and a grep that matched nothing both print
#     nothing. So an empty and a missing snapshot must both be REFUSED, not
#     reported clean.
#
# The last case is the live one: the snapshot assembled from the CURRENT
# tracked tree must be clean. That is what turns this from a unit test of a
# script into a gate on the repository -- commit a lab IP into docs/ and this
# goes red before it can reach a public repo, which is the one failure that
# cannot be recalled.
#
# Host-only: no QEMU, no compiler, no network.
#
# Usage: ./test/integration/test-source-snapshot.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

MAKE="$PROJECT_DIR/scripts/make-source-snapshot.sh"
CHECK="$PROJECT_DIR/scripts/check-snapshot-clean.py"
WORK="$(mktemp -d -t axl-snap.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

check() {
    if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
    return 0
}

echo "=== public source snapshot ==="
echo ""

for f in "$MAKE" "$CHECK"; do
    [[ -x "$f" ]] || { test_host_fail "$(basename "$f") is executable"
                       test_host_summary "source-snapshot"; exit 1; }
done
test_host_pass "the snapshot scripts are executable"

# ---------------------------------------------------------------------------
# The real thing: assembled from the tracked tree as it stands.
# ---------------------------------------------------------------------------
SNAP="$WORK/snap"
if ! "$MAKE" --out "$SNAP" --from worktree > "$WORK/make.log" 2>&1; then
    test_host_fail "assembles a snapshot from the tracked tree"
    tail -5 "$WORK/make.log" | sed 's/^/      /'
    test_host_summary "source-snapshot"; exit 1
fi
test_host_pass "assembles a snapshot from the tracked tree"

"$CHECK" "$SNAP" > "$WORK/clean.log" 2>&1
rc=$?
[[ "$rc" -eq 0 ]]
check $? "the snapshot we would publish carries no private infrastructure"
[[ "$rc" -eq 0 ]] || sed 's/^/      /' "$WORK/clean.log" | head -12

# ---------------------------------------------------------------------------
# The exclusions actually excluded. Asserted on the ASSEMBLED tree, because
# that is the thing that gets pushed -- a check on the source tree would pass
# while the exclusion silently stopped working.
# ---------------------------------------------------------------------------
for gone in ".github" "docs/superpowers" "docs/HW-Testing-Workflow.md"; do
    [[ ! -e "$SNAP/$gone" ]]
    check $? "the snapshot excludes $gone"
done
n_handoffs=$(find "$SNAP/docs" -maxdepth 1 -name 'AXL-Session-Handoff-*.md' 2>/dev/null | wc -l)
[[ "$n_handoffs" -eq 0 ]]
check $? "the snapshot excludes the session handoffs (found $n_handoffs)"

# ...and still carries what consumers need, or the exclusions went too far.
for want in "README.md" "LICENSE" "include/axl.h" "src/mem" "docs/AXL-Design.md"; do
    [[ -e "$SNAP/$want" ]]
    check $? "the snapshot still carries $want"
done

# ---------------------------------------------------------------------------
# CONTROLS. Each forbidden class, planted, and the check must SEE it.
# ---------------------------------------------------------------------------
plant() {  # plant <relative-path> <content> ; echoes a fresh snapshot dir
    local dir; dir="$(mktemp -d -p "$WORK")"
    cp -a "$SNAP/." "$dir/"
    mkdir -p "$dir/$(dirname "$1")"
    printf '%s\n' "$2" > "$dir/$1"
    echo "$dir"
}

# A private address in prose -- the shape of the leak §15.1's own inventory
# missed (a lab iDRAC in an archived roadmap). The address here is SYNTHETIC on
# purpose: an earlier draft planted the real one, which put it straight back
# into a tracked file and undid the redaction it was testing.
D=$(plant "docs/planted-note.md" "the box answers on 10.99.99.99 today")
"$CHECK" "$D" > "$WORK/c1.log" 2>&1
[[ $? -ne 0 ]] && grep -q "docs/planted-note.md" "$WORK/c1.log"
check $? "control: a private address in prose is caught and located"

# The same address in a TEST fixture must NOT fire -- that is an input, not a
# machine, and a gate that cries wolf over test data gets switched off.
#
# Asserted as "this FILE is not reported", not as "the whole run is clean".
# The overall-cleanliness form only means anything while the base snapshot is
# spotless, so the first unrelated finding anywhere cascaded into a failure
# here and blamed the wrong thing -- which is exactly what happened when a
# private address reached a design doc.
D=$(plant "test/unit/planted-fixture.c" 'static const char *ip = "10.99.99.99";')
"$CHECK" "$D" > "$WORK/c2.log" 2>&1
! grep -q "planted-fixture.c" "$WORK/c2.log"
check $? "control: the same address in a test fixture does NOT fire"

# An ssh tunnel directive, anywhere.
D=$(plant "docs/planted-conf.md" "    ProxyJump someuser@somehost")
"$CHECK" "$D" > "$WORK/c3.log" 2>&1
[[ $? -ne 0 ]] && grep -q "ProxyJump" "$WORK/c3.log"
check $? "control: an ssh ProxyJump directive is caught"

# A string supplied at release time as a secret, never committed here.
D=$(plant "docs/planted-host.md" "deployed to zzz-not-a-real-host-zzz last week")
AXL_SNAPSHOT_FORBIDDEN="zzz-not-a-real-host-zzz" "$CHECK" "$D" > "$WORK/c4.log" 2>&1
rc=$?
[[ "$rc" -ne 0 ]] && grep -q "planted-host.md" "$WORK/c4.log"
check $? "control: a string from AXL_SNAPSHOT_FORBIDDEN is caught"
# ...and the secret itself must never be echoed into a public build log.
! grep -q "zzz-not-a-real-host-zzz" "$WORK/c4.log"
check $? "control: the forbidden string is NOT reproduced in the output"

# ---------------------------------------------------------------------------
# "Could not run" must not read as "found nothing".
# ---------------------------------------------------------------------------
"$CHECK" "$WORK/does-not-exist" > "$WORK/c5.log" 2>&1
[[ $? -ne 0 ]] && grep -q "not a directory" "$WORK/c5.log"
check $? "a missing snapshot directory is refused, not reported clean"

mkdir -p "$WORK/tiny" && echo hi > "$WORK/tiny/a.md"
"$CHECK" "$WORK/tiny" > "$WORK/c6.log" 2>&1
[[ $? -ne 0 ]] && grep -q "not a source snapshot" "$WORK/c6.log"
check $? "a near-empty snapshot is refused, not reported clean"

test_host_summary "source-snapshot"
