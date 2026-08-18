#!/bin/bash
# release-gate.sh — is the working tree already known green?
#
# Sourced by run-integration.sh (to WRITE the stamp) and by cut-release.sh (to
# READ it). One file so the two cannot drift on the format, which is the whole
# reason a shared helper exists rather than two greps.
#
# The stamp answers one question: "did the authoritative local suite finish
# clean on THIS tree?" A release that can answer yes needs no CI dispatch, and
# the cheapest run is the one never dispatched — the only move that improves
# turnaround and cost at the same time (AXL-CI-Release-Speed-Design.md §10.4).
#
# NOT committed: it describes one machine's run, not a property of the tree.

AXL_STAMP_FILE="${AXL_STAMP_FILE:-test/integration/.last-run-stamp}"

# release_gate_write <sha> <arch> <pass> <fail>
release_gate_write() {
    local sha=$1 arch=$2 pass=$3 fail=$4
    printf 'sha=%s\narch=%s\npass=%s\nfail=%s\nwhen=%s\n' \
        "$sha" "$arch" "$pass" "$fail" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        > "$AXL_STAMP_FILE"
}

# The three files a `release: vX.Y.Z` commit is allowed to touch. A release
# commit that changes only these is covered by its PARENT's green run — the
# stamp is written against HEAD BEFORE the bump, so without this the stamp can
# never match at cut time (design §10.3). Keep in sync with cut-release.sh's
# bump step; a fourth file appearing here must be a deliberate decision.
AXL_RELEASE_ONLY_FILES=(VERSION include/axl/axl-version.h CHANGELOG.md)

# release_gate_covers <sha> — 0 if the stamp covers <sha>, else 1 with a reason
# on stdout. Accepts the stamped SHA itself, or a descendant that differs from
# the stamped commit only in AXL_RELEASE_ONLY_FILES.
release_gate_covers() {
    local want=$1 sha arch pass fail when
    [[ -f "$AXL_STAMP_FILE" ]] || { echo "no stamp at $AXL_STAMP_FILE"; return 1; }
    # shellcheck disable=SC1090
    sha=$(sed -n 's/^sha=//p'  "$AXL_STAMP_FILE")
    arch=$(sed -n 's/^arch=//p' "$AXL_STAMP_FILE")
    fail=$(sed -n 's/^fail=//p' "$AXL_STAMP_FILE")
    pass=$(sed -n 's/^pass=//p' "$AXL_STAMP_FILE")
    when=$(sed -n 's/^when=//p' "$AXL_STAMP_FILE")

    [[ -n "$sha" && -n "$fail" ]] || { echo "stamp is malformed"; return 1; }
    [[ "$fail" == "0" ]] || { echo "stamped run had $fail failure(s)"; return 1; }

    if [[ "$sha" == "$want" ]]; then
        echo "stamp matches HEAD ($pass passed, $arch, $when)"
        return 0
    fi

    # Not the same commit. Covered only if it is a descendant whose entire diff
    # is the version bump.
    git merge-base --is-ancestor "$sha" "$want" 2>/dev/null \
        || { echo "stamped $sha is not an ancestor of $want"; return 1; }

    local changed extra
    changed=$(git diff --name-only "$sha" "$want" 2>/dev/null) || {
        echo "cannot diff $sha..$want"; return 1; }
    [[ -n "$changed" ]] || { echo "stamp matches HEAD ($pass passed, $arch, $when)"; return 0; }

    extra=$(comm -23 <(printf '%s\n' "$changed" | sort -u) \
                     <(printf '%s\n' "${AXL_RELEASE_ONLY_FILES[@]}" | sort -u))
    if [[ -n "$extra" ]]; then
        echo "tree changed since the stamped run: $(printf '%s ' $extra)"
        return 1
    fi
    echo "stamp covers the parent ($pass passed, $arch, $when); the release commit changes only the version files"
    return 0
}
