#!/bin/bash
# test-meta: arch=none needs= est=2 local-only=0
# test-cut-release-branch.sh — `cut-release.sh --branch` refuses any branch
# whose name does not encode the version being cut.
#
# The flag exists because `main` is often mid-feature when a released user
# needs a fix: RELEASING.md's "Cutting from a tag" flow (v3.2.1, v3.2.2 and
# v3.2.3 were all cut that way) was a by-hand procedure precisely because the
# script hard-required `main` and hardcoded `git push origin main` — running it
# from a release branch would have pushed that branch's commits onto main.
#
# Parameterising the branch re-opens exactly that danger, so the guard is the
# branch NAME: it must spell the version being cut (`release-X.Y.Z`), or name
# the minor line a patch belongs to (`release/X.Y.x`). Every case below is a
# refusal; the one acceptance asserts only that validation was PASSED (the run
# reaches the gh precondition), because going further would push.
#
# Host-only: no QEMU, no build, no network — every refusal happens before the
# script's first `gh` call.
#
# Usage: ./test/integration/test-cut-release-branch.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# Overridable so the structural assertions below can be pointed at another copy
# of the script -- which is how they were shown to FAIL against the draft that
# forced the gate without dispatching it.
CUT="${AXL_CUT_RELEASE:-$PROJECT_DIR/scripts/cut-release.sh}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Run the script with args and capture stderr+stdout. It never gets past
# validation in the refusal cases, so nothing is pushed and no tag is made.
run_cut() { "$CUT" "$@" 2>&1 || true; }

# $1 label, $2 expected substring, rest: args
refuses() {
    local label="$1" want="$2"; shift 2
    local out; out="$(run_cut "$@")"
    if [[ "$out" == *"$want"* ]]; then
        pass "$label"
    else
        fail "$label (wanted '$want', got: $(head -2 <<<"$out" | tr '\n' ' '))"
    fi
}

echo "=== cut-release.sh --branch validation ==="

refuses "a branch that names no version is refused" \
    "must be main, release-X.Y.Z or release/X.Y.x" \
    4.7.2 --branch feature/whatever
refuses "release-X.Y.Z must name the version being cut" \
    "does not name the version being cut" \
    4.7.3 --branch release-4.7.2
refuses "a minor may not be cut from a maintenance line" \
    "may only cut patches of 4.7" \
    4.8.0 --branch release/4.7.x
refuses "a maintenance line may not cut another minor's patch" \
    "may only cut patches of 4.7" \
    4.6.2 --branch release/4.7.x
refuses "X.Y.0 is not a patch of a maintenance line" \
    "may only cut patches of 4.7" \
    4.7.0 --branch release/4.7.x
refuses "--branch without a value is refused" \
    "--branch needs a branch name" \
    4.7.2 --branch
refuses "an unknown flag is still refused" \
    "unknown flag" \
    4.7.2 --bogus
refuses "a malformed version is still refused" \
    "version must be X.Y.Z" \
    4.7 --branch release-4.7

# The acceptance: a well-formed pair passes validation and reaches the first
# precondition. Asserted with a STUB gh that fails its API probe, so the run
# stops at "gh cannot reach the API" — which proves validation let it through
# while keeping the test hermetic and far from anything that pushes. (Blanking
# PATH instead would take `dirname` with it and the script would die earlier,
# for a reason that says nothing about the branch validation.)
STUB_BIN="$(mktemp -d)"
trap 'rm -rf "$STUB_BIN"' EXIT
printf '#!/bin/sh\nexit 1\n' > "$STUB_BIN/gh"
chmod +x "$STUB_BIN/gh"

accepted() {
    local label="$1"; shift
    local out; out="$(PATH="$STUB_BIN:$PATH" "$CUT" "$@" 2>&1 || true)"
    if [[ "$out" == *"gh cannot reach the API"* ]]; then
        pass "$label"
    else
        fail "$label (expected to reach the gh precondition, got: $(head -3 <<<"$out" | tr '\n' ' '))"
    fi
}

accepted "release-X.Y.Z naming its own version passes validation" \
    4.7.2 --branch release-4.7.2
accepted "a patch of a maintenance line passes validation" \
    4.7.2 --branch release/4.7.x
accepted "--branch=NAME is accepted too" \
    4.7.2 --branch=release-4.7.2
accepted "no --branch still means main" 4.8.0

# --------------------------------------------------------------------------
# The other half of the flag: a non-main branch must DISPATCH the CI it gates
# on. Structural, like test-release-gate.sh's own contract checks -- the
# behaviour needs a live workflow, but the shape is what broke: the first draft
# forced CI_GATE=true for --branch and the --resume path then only WAITED, so a
# resume on a release branch polled 75 minutes for a run that ci.yml has no
# trigger to create. The recovery text printed on a failed cut sends you to
# exactly that command.
# --------------------------------------------------------------------------
echo ""
echo "=== the forced CI gate dispatches ==="

resume_block() {
    # From `if $RESUME; then` to the `fi` that closes it (the next line that is
    # a bare `fi` at column 0).
    awk '/^if \$RESUME; then$/{f=1} f{print} f&&/^fi$/{exit}' "$CUT"
}

if grep -q 'CI_GATE=true' <<<"$(awk '/^if \[\[ "\$RELEASE_BRANCH" != "main" \]\]; then$/{f=1} f{print} f&&/^fi$/{exit}' "$CUT")"; then
    pass "a non-main --branch forces the CI gate on"
else
    fail "a non-main --branch no longer forces the CI gate"
fi

if grep -q 'gh workflow run ci.yml --ref "\$RELEASE_BRANCH"' <<<"$(resume_block)"; then
    pass "--resume dispatches CI on the release branch (it waits on a run a push cannot start)"
else
    fail "--resume waits on CI without dispatching it — a release-branch resume would hang"
fi

# ...ON MAIN TOO (AXL-CI-Release-Speed-Design.md §14). The dispatch used to be
# for non-main branches only, because a push to main ran the suite. It no
# longer does, so a main resume that only waited would poll 75 minutes for a
# suite run nothing starts.
if grep -qF '!= "main"' <<<"$(resume_block)"; then
    fail "--resume still limits the dispatch to non-main branches -- a main resume would hang"
else
    pass "--resume dispatches on main as well (a push to main runs no suite)"
fi

# Skips the dispatch when something already covers the sha: the resume asks
# the same release_gate_decide the normal flow does, which accepts a green
# dispatched run (review ruling R-B)...
if grep -q 'release_gate_decide' <<<"$(resume_block)"; then
    pass "--resume asks the release gate first (a covered sha is not re-dispatched)"
else
    fail "--resume does not consult the release gate — a covered sha would re-run CI"
fi
# ...and a dispatched run still going is WAITED on, not replaced: dispatches
# share a cancel-in-progress group, so a second dispatch would cancel it.
if grep -q 'queued|in_progress|waiting|requested|pending)' <<<"$(resume_block)"; then
    pass "--resume waits on a dispatched run still in progress"
else
    fail "--resume re-dispatches over a run in progress — it would cancel it"
fi

# The gate itself must not accept a run in which every job was skipped: on a
# release branch the dispatched run is the ONLY source of green.
if awk '/^wait_for_ci\(\) \{/{f=1} f{print} f&&/^\}$/{exit}' "$CUT" \
     | grep -q '_release_gate_ci_green'; then
    pass "wait_for_ci asserts the suite job ran, not just run-level success"
else
    fail "wait_for_ci accepts run-level success alone (an all-skipped run would pass)"
fi

echo ""
echo "cut-release --branch: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
