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

# FOUND FROM THIS FILE, not from the repo being examined. The first version
# used `git rev-parse --show-toplevel`, which is the tree the QUESTION is about
# -- a fixture repo under /tmp during a test, and any consumer's checkout in
# principle. It resolved to a path with no scripts/ in it, the helper returned
# "cannot answer", and the walk below silently never accepted anything: a gate
# that fails closed for a reason unrelated to the tree.
AXL_GATE_LIB_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AXL_CI_PLAN="${AXL_CI_PLAN:-$AXL_GATE_LIB_DIR/../../../scripts/ci-plan.sh}"

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

# THE JOB THAT MAKES A CI RUN COUNT AS A GATE. Named, because "every job
# succeeded" stopped being sufficient the moment a push could produce a run
# that legitimately contains only SOME jobs: a docs-only push runs the docs job
# and nothing else, every job in it succeeds, and without this the gate would
# read that as "CI is green" and tag code the suite never touched.
#
# A second spelling of ci.yml's job name, which is the drift shape this tree
# pays for -- so test-release-gate.sh holds the two equal, the same way it does
# for the [release-cut] marker. They live in files that cannot import each
# other.
AXL_CI_SUITE_JOB="${AXL_CI_SUITE_JOB:-QEMU integration (full suite)}"

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
    # is the version bump -- the rule below, shared with the CI half so the two
    # cannot come to disagree about what a release commit is allowed to touch.
    local why
    why=$(release_gate_release_only "$sha" "$want") || { echo "$why"; return 1; }
    echo "stamp covers the parent ($pass passed, $arch, $when); the release commit changes only the version files"
    return 0
}

# release_gate_release_only <base> <want> — 0 if <want> is <base>, or a
# descendant of it whose entire diff is AXL_RELEASE_ONLY_FILES. On refusal it
# prints the reason to stdout and returns 1.
#
# Extracted from release_gate_covers so the CI half asks the same question of
# the same commits. Two copies of "is this only a version bump?" is the shape
# that decides whether a release is gated at all.
release_gate_release_only() {
    local base=$1 want=$2 changed extra
    [[ "$base" == "$want" ]] && return 0
    git merge-base --is-ancestor "$base" "$want" 2>/dev/null \
        || { echo "$base is not an ancestor of $want"; return 1; }
    changed=$(git diff --name-only "$base" "$want" 2>/dev/null) || {
        echo "cannot diff $base..$want"; return 1; }
    [[ -n "$changed" ]] || return 0
    extra=$(comm -23 <(printf '%s\n' "$changed" | sort -u) \
                     <(printf '%s\n' "${AXL_RELEASE_ONLY_FILES[@]}" | sort -u))
    if [[ -n "$extra" ]]; then
        # shellcheck disable=SC2086  # deliberate word split: one name per token
        echo "tree changed since $base: $(printf '%s ' $extra)"
        return 1
    fi
    return 0
}

# ── the CI half ───────────────────────────────────────────────────────────
#
# WHY THIS EXISTS. `ci.yml` runs the identical uncached suite on every push to
# `main`, on the same self-hosted box, and goes green on the commit a release
# is cut from. The stamp above cannot see any of that -- it reads a local file
# that only an uncached LOCAL run writes -- so the only way to satisfy the gate
# was to run that suite a second time on the same hardware (~8.5 min, with a
# human waiting), and `--ci-gate` would then dispatch a THIRD run rather than
# consult the green one that already exists.
#
# This does NOT weaken the gate. The run it reuses is the same suite, on the
# same machine, on the same tree.

# _release_gate_ci_green <sha> — 0 if a CI run at <sha> completed with EVERY
# job successful. Quiet; the caller does the reporting.
_release_gate_ci_green() {
    local sha=$1 id="" h st cc did jc n=0
    # THE RUN MUST BE AT THE SHA WE ASKED ABOUT. `gh run list --commit` filters
    # server-side, so this is belt and braces against a stub, a cached answer,
    # or a future flag change -- and it is free.
    # `|| [[ -n "$h" ]]`: `read` returns non-zero on a final line with no
    # trailing newline and STILL assigns, so a bare `while read` silently drops
    # the last record. Here that record is the whole answer.
    while IFS=$'\t' read -r h st cc did || [[ -n "$h" ]]; do
        [[ "$h" == "$sha" && "$st" == "completed" && "$cc" == "success" ]] || continue
        id="$did"
        break
    done < <(gh run list --workflow=ci.yml --commit "$sha" --limit 10 \
                 --json headSha,status,conclusion,databaseId \
                 --jq '.[] | [.headSha, .status, .conclusion, .databaseId] | @tsv' \
             2>/dev/null)
    [[ -n "$id" ]] || return 1

    # EVERY JOB MUST HAVE SUCCEEDED, and this is the assertion that matters
    # most. The `release: vX.Y.Z` commit carries a `[release-cut]` marker and
    # every ci.yml job is conditioned on NOT seeing it -- so a CI run DOES
    # exist at that commit and it tested nothing. A run-level conclusion alone
    # would accept it, which is precisely "tag code no suite ever ran", by the
    # most ordinary path there is.
    #
    # And a run reporting NO jobs is refused too: an empty answer from a query
    # that could not run looks identical to one from a run that had none, and
    # only one of those is safe to treat as green.
    local saw_suite=0 jn
    while IFS=$'\t' read -r jn jc || [[ -n "$jn" ]]; do
        [[ -n "$jn" ]] || continue
        n=$((n + 1))
        [[ "$jc" == "success" ]] || return 1
        [[ "$jn" == "$AXL_CI_SUITE_JOB" ]] && saw_suite=1
    done < <(gh run view "$id" --json jobs \
                 --jq '.jobs[] | [.name, .conclusion] | @tsv' 2>/dev/null)
    [[ "$n" -gt 0 ]] || return 1
    # ...AND THE SUITE MUST BE AMONG THEM. See AXL_CI_SUITE_JOB: a run of jobs
    # that all passed is not the same claim as a run that tested the tree.
    [[ "$saw_suite" -eq 1 ]] || return 1
    return 0
}

# release_gate_diff_inert <base> <want> — 0 if everything that changed between
# them is either the version bump or prose.
#
# TWO LISTS, COMPOSED, NEITHER COPIED. scripts/ci-plan.sh owns which paths are
# safe prose (and refuses a deletion); AXL_RELEASE_ONLY_FILES above owns which
# files a release commit may touch. This asks ci-plan for what is NOT prose and
# checks the remainder is only version files. Re-stating either list here would
# put a second copy of it in the file that decides whether a tag is verified.
release_gate_diff_inert() {
    local base=$1 want=$2 unsafe extra
    [[ -x "$AXL_CI_PLAN" ]] || return 1
    unsafe=$("$AXL_CI_PLAN" --list-unsafe "$base" "$want") || return 1
    [[ -n "$unsafe" ]] || return 0
    extra=$(comm -23 <(printf '%s\n' "$unsafe" | sort -u) \
                     <(printf '%s\n' "${AXL_RELEASE_ONLY_FILES[@]}" | sort -u))
    [[ -z "$extra" ]]
}

# release_gate_ci_covers <sha> — 0 if a green CI run covers <sha>, else 1 with
# a reason on stdout. The commit itself, then its parent under the release-only
# rule, then a bounded walk back for the case ci-plan.sh created.
#
# WHY THE WALK EXISTS. A docs-only push now skips every job, so after one there
# is no green run at the tip -- and this gate refused, sending the cut back to
# the ~8.5-minute local suite the CI half had just removed. The two mechanisms
# fought, and the newer one shipped first. Accepting it is consistent rather
# than lenient: "this diff cannot affect the suite" is precisely what ci-plan
# asserts, and test-ci-plan.sh sabotage-verifies that claim.
#
# BOUNDED, because "walk back until something is green" with no limit is a way
# to tag anything at all. Every step of the walk still has to pass
# release_gate_diff_inert, so the bound is a backstop rather than the guard.
AXL_CI_WALK_MAX="${AXL_CI_WALK_MAX:-25}"
release_gate_ci_covers() {
    local want=$1
    # "gh is not installed" and "gh says there is no green run" are the same
    # empty string and opposite facts, so the tool's absence is answered here
    # rather than read out of an empty result.
    command -v gh >/dev/null 2>&1 || { echo "gh is not on PATH, so CI cannot be consulted"; return 1; }

    if _release_gate_ci_green "$want"; then
        echo "CI is green on $want"
        return 0
    fi
    # NO SEPARATE PARENT STEP. There was one, and it RETURNED when the parent's
    # diff was not release-only -- so a docs-only push, whose parent diff is
    # prose, never reached the walk that exists for exactly that case. The walk
    # starts at $want itself and applies the more general test at every step,
    # so the parent is simply its second iteration.
    _release_gate_walk_back "$want"
}

# The bounded walk. Separate from the caller so the two-step above reads as the
# common case it is, and so this one can say WHICH ancestor answered.
_release_gate_walk_back() {
    local want=$1 n=0 c
    while read -r c; do
        [[ -n "$c" ]] || continue
        n=$((n + 1))
        [[ "$n" -le "$AXL_CI_WALK_MAX" ]] || break
        release_gate_diff_inert "$c" "$want" || continue
        if _release_gate_ci_green "$c"; then
            echo "CI is green on $c, and everything since is prose or the version bump"
            return 0
        fi
    done < <(git rev-list --max-count="$AXL_CI_WALK_MAX" "$want" 2>/dev/null)
    echo "no green CI run within $AXL_CI_WALK_MAX commits whose diff to $want is inert"
    return 1
}
