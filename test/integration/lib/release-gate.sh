#!/bin/bash
# release-gate.sh — is the working tree already known green?
#
# Sourced by scripts/release-suite.sh (to WRITE the stamp) and by
# cut-release.sh and check-main-ci.sh (to READ it and ask CI). One file so the
# writers and readers cannot drift on the format, which is the whole reason a
# shared helper exists rather than two greps.
#
# The stamp answers one question: "did the release suite -- the SAME steps
# CI's integration job runs (scripts/release-suite.sh) -- finish clean on THIS
# commit?" A bare run-integration.sh run no longer writes it: it certified
# less than the CI job it stood in for (review ruling R-A, design §14). A
# release that can answer yes needs no CI dispatch, and
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

# release_gate_write <sha> <arch> -- the stamp of a COMPLETE release-suite run.
# Only scripts/release-suite.sh calls it, after every step passed on a clean
# tree at <sha>; `suite=release-suite` is what release_gate_covers requires,
# so a stamp in the old run-integration.sh format is refused by name.
release_gate_write() {
    local sha=$1 arch=$2
    printf 'sha=%s\nsuite=release-suite\narch=%s\nwhen=%s\n' \
        "$sha" "$arch" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$AXL_STAMP_FILE"
}

# The three files a `release: vX.Y.Z` commit is allowed to touch. A release
# commit that changes only these is covered by its PARENT's green run — the
# stamp is written against HEAD BEFORE the bump, so without this the stamp can
# never match at cut time (design §10.3). Keep in sync with cut-release.sh's
# bump step; a fourth file appearing here must be a deliberate decision.
AXL_RELEASE_ONLY_FILES=(VERSION include/axl/axl-version.h CHANGELOG.md)

# THE JOBS THAT MAKE A CI RUN COUNT, BY NAME. "Every job succeeded" stopped
# being sufficient the moment a push could produce a run that legitimately
# contains only SOME jobs, and since 2026-10-21 EVERY push does: a push runs
# `plan` and the three cheap jobs below, while the suite job is listed SKIPPED
# because it runs only on workflow_dispatch (AXL-CI-Release-Speed-Design.md
# §14). Such a run concludes success, and reading that as "CI is green" would
# tag code the suite never touched.
#
# Second spellings of ci.yml's job names, which is the drift shape this tree
# pays for -- so test-release-gate.sh holds them equal, the same way it does
# for the [release-cut] marker. They live in files that cannot import each
# other. `gcc x64`/`gcc aa64` are the build job's matrix expansion of
# `gcc ${{ matrix.arch }}`.
AXL_CI_SUITE_JOB="${AXL_CI_SUITE_JOB:-QEMU integration (full suite)}"
AXL_CI_CHEAP_JOBS=("gcc x64" "gcc aa64" "clang-tidy")

# release_gate_covers <sha> — 0 if the stamp covers <sha>, else non-zero with
# a reason on stdout. Accepts the stamped SHA itself, or a descendant that
# differs from the stamped commit only in AXL_RELEASE_ONLY_FILES.
#
# AND THE CHEAP PUSH CI MUST BE GREEN ON THAT TREE (§14). Since the suite left
# the push trigger, a local uncached run is one of the two ways to certify a
# release -- but only together with a green push run of the three cheap jobs,
# because those catch what a dev box STRUCTURALLY cannot: a clean checkout
# that does not link (§11.2, 2026-08-18). Asked through the same bounded,
# inertness-checked walk as the CI half, so a docs-only push at the tip (whose
# run skips the cheap jobs) carries the green of the commit below it.
#
# Exit status: 0 covered; 1 not covered; 2 the stamp is good but CI COULD NOT
# BE CHECKED (no gh, a failing gh). Both refuse; only one of them means "go
# and push something".
release_gate_covers() {
    local want=$1 sha suite arch when stamp_why ci_why rc
    [[ -f "$AXL_STAMP_FILE" ]] || { echo "no stamp at $AXL_STAMP_FILE"; return 1; }
    sha=$(sed -n 's/^sha=//p'  "$AXL_STAMP_FILE")
    suite=$(sed -n 's/^suite=//p' "$AXL_STAMP_FILE")
    arch=$(sed -n 's/^arch=//p' "$AXL_STAMP_FILE")
    when=$(sed -n 's/^when=//p' "$AXL_STAMP_FILE")

    [[ -n "$sha" ]] || { echo "stamp is malformed"; return 1; }
    [[ "$suite" == "release-suite" ]] || {
        echo "stamp was not written by scripts/release-suite.sh -- a bare run-integration.sh run does not certify a release"
        return 1; }

    if [[ "$sha" == "$want" ]]; then
        stamp_why="stamp matches HEAD (release-suite, $arch, $when)"
    else
        # Not the same commit. Covered only if it is a descendant whose entire
        # diff is the version bump -- the rule below, shared with the CI half
        # so the two cannot come to disagree about what a release commit is
        # allowed to touch.
        local why
        why=$(release_gate_release_only "$sha" "$want") || { echo "$why"; return 1; }
        stamp_why="stamp covers the parent (release-suite, $arch, $when); the release commit changes only the version files"
    fi

    ci_why=$(_release_gate_walk_back "$want" _release_gate_ci_cheap_green \
                 "cheap push CI ($(_release_gate_join ', ' "${AXL_CI_CHEAP_JOBS[@]}"))")
    rc=$?
    case $rc in
        0) printf '%s\n%s\n' "$stamp_why" "$ci_why" ;;
        2) printf '%s, but the cheap push CI could not be checked\n%s\n' "$stamp_why" "$ci_why" ;;
        3) printf '%s, but the cheap push CI is still running\n%s\n' "$stamp_why" "$ci_why" ;;
        *) printf '%s, but the cheap push CI does not cover it\n%s\n' "$stamp_why" "$ci_why" ;;
    esac
    return "$rc"
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
# WHY THIS EXISTS. A ci.yml run that contains the suite -- since §14 that is a
# DISPATCHED run (`gh workflow run ci.yml --ref main`, by hand or by
# cut-release.sh --ci-gate) -- is the same uncached suite on the same box. The
# stamp above cannot see it, because it reads a local file that only a LOCAL
# run writes, so without this a green dispatched run would still send the cut
# to a second local run.
#
# This does NOT weaken the gate. The run it reuses is the same suite, on the
# same machine, on the same tree.

# AXL_GH -- the gh to run. A seam for test-release-gate.sh, which has to take
# gh away while keeping sed and git: PATH=/nonexistent removes all three, and
# the stamp half then refuses as "malformed" -- the right status, wrong reason.
_release_gate_gh() { "${AXL_GH:-gh}" "$@"; }

# _release_gate_join <sep> <item>... -- the items joined by <sep>.
_release_gate_join() {
    local sep=$1 out="" x
    shift
    for x in "$@"; do out+="${out:+$sep}$x"; done
    printf '%s' "$out"
}

# _release_gate_ci_jobs_green <sha> <job>... — 0 if SOME completed, successful
# CI run at <sha> lists every named job, each concluding `success`. On refusal
# prints ONE line of reason and returns 1 (no such run) or 2 (could not ask).
#
# EVERY GREEN RUN IS EXAMINED, not the first. Since §14 one commit routinely
# carries two runs: its push run (cheap jobs, suite skipped) and a dispatched
# one (everything). The push run is often the newer, and stopping at it
# refused a commit the suite HAD covered.
#
# "COULD NOT ASK" IS ITS OWN ANSWER. `gh` missing, an expired token and a
# throttled API all produce an empty listing -- the same empty string as
# "there is no run" and the opposite fact -- so the exit status of every
# query is read, and a failure is 2, never 1.
_release_gate_ci_jobs_green() {
    local sha=$1; shift
    local gh="${AXL_GH:-gh}" runs jobs h st cc did jn jc req found
    local reasons=() could_not=0 any=0 bad running="" cancelled=""
    command -v "$gh" >/dev/null 2>&1 || { echo "gh is not available ($gh not found)"; return 2; }

    runs=$(_release_gate_gh run list --workflow=ci.yml --commit "$sha" --limit 20 \
               --json headSha,status,conclusion,databaseId \
               --jq '.[] | [.headSha, .status, .conclusion, .databaseId] | @tsv' \
               2>/dev/null) || { echo "\`gh run list\` failed"; return 2; }

    # `|| [[ -n "$h" ]]`: `read` returns non-zero on a final line with no
    # trailing newline and STILL assigns, so a bare `while read` silently
    # drops the last record -- and here that record can be the whole answer.
    while IFS=$'\t' read -r h st cc did || [[ -n "$h" ]]; do
        # THE RUN MUST BE AT THE SHA WE ASKED ABOUT. `--commit` filters
        # server-side, so this is belt and braces against a stub, a cached
        # answer, or a future flag change -- and it is free.
        [[ -n "$h" && "$h" == "$sha" ]] || continue
        any=1
        [[ "$st" == "completed" ]] || { running="$st"; continue; }
        # A CANCELLED run is remembered: a docs-only push cancels the code
        # push's run below it (cancel-in-progress), and "cancelled; re-dispatch
        # or re-push" is a different remedy from "it failed".
        [[ "$cc" == "cancelled" ]] && cancelled="$did"
        [[ "$cc" == "success" ]] || continue
        if ! jobs=$(_release_gate_gh run view "$did" --json jobs \
                        --jq '.jobs[] | [.name, .conclusion] | @tsv' 2>/dev/null); then
            reasons+=("\`gh run view $did\` failed"); could_not=1; continue
        fi
        # A run reporting NO jobs is refused: an empty answer from a query that
        # could not run looks identical to one from a run that had none, and
        # only one of those is safe to treat as green. (GitHub has been seen
        # to list a run completed while its jobs array was still empty.)
        if [[ -z "${jobs//[[:space:]]/}" ]]; then
            reasons+=("run $did reported no jobs"); continue
        fi
        bad=""
        for req in "$@"; do
            found=""
            while IFS=$'\t' read -r jn jc || [[ -n "$jn" ]]; do
                [[ "$jn" == "$req" ]] && { found="$jc"; break; }
            done <<< "$jobs"
            if [[ -z "$found" ]]; then
                bad="run $did did not run '$req'"; break
            elif [[ "$found" != "success" ]]; then
                bad="run $did: '$req' concluded $found"; break
            fi
        done
        [[ -z "$bad" ]] && return 0
        reasons+=("$bad")
    done <<< "$runs"

    # Precedence: could not ask (2) > still running (3) > not covered (1).
    # A run still going is the commonest case right after a push -- or a
    # dispatch -- and its remedy (wait) is the opposite of "no run". Exit 3 so
    # a caller can wait rather than refuse.
    if [[ $could_not -eq 1 ]]; then
        _release_gate_join '; ' "${reasons[@]}"; echo
        return 2
    fi
    if [[ -n "$running" ]]; then
        echo "a CI run is still $running -- wait for it"
        return 3
    fi
    # Every green run either returned above or left a reason, so an empty
    # list means no green run was seen at all.
    if [[ ${#reasons[@]} -gt 0 ]]; then
        _release_gate_join '; ' "${reasons[@]}"; echo
    elif [[ -n "$cancelled" ]]; then
        echo "run $cancelled was cancelled -- re-dispatch or re-push"
    elif [[ $any -eq 1 ]]; then
        echo "no successful CI run"
    else
        echo "no CI run"
    fi
    return 1
}

# _release_gate_ci_green <sha> — a FULL run went green at <sha>: the suite job
# and the three cheap jobs, by name. This is what certifies a release on its
# own; since §14 only a dispatched run can satisfy it.
_release_gate_ci_green() {
    _release_gate_ci_jobs_green "$1" "${AXL_CI_CHEAP_JOBS[@]}" "$AXL_CI_SUITE_JOB"
}

# _release_gate_ci_cheap_green <sha> — the cheap PUSH jobs went green at <sha>.
# Not a release gate alone: it is the half a local stamp needs (§14), and what
# check-main-ci.sh reports per push.
_release_gate_ci_cheap_green() {
    _release_gate_ci_jobs_green "$1" "${AXL_CI_CHEAP_JOBS[@]}"
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

# release_gate_ci_covers <sha> — 0 if a green FULL CI run covers <sha>; else 1
# (not covered) or 2 (could not check), with the reason on stdout. The commit
# itself, then its parent under the release-only rule, then a bounded walk
# back for the case ci-plan.sh created.
#
# WHY THE WALK EXISTS. A docs-only push skips every job, so after one there is
# no green run at the tip -- and this gate refused, sending the cut back to the
# ~8.5-minute local suite the CI half had just removed. The two mechanisms
# fought, and the newer one shipped first. Accepting it is consistent rather
# than lenient: "this diff cannot affect the suite" is precisely what ci-plan
# asserts, and test-ci-plan.sh sabotage-verifies that claim.
#
# BOUNDED, because "walk back until something is green" with no limit is a way
# to tag anything at all. Every step of the walk still has to pass
# release_gate_diff_inert, so the bound is a backstop rather than the guard.
AXL_CI_WALK_MAX="${AXL_CI_WALK_MAX:-25}"
release_gate_ci_covers() {
    local want=$1 why rc
    # NO SEPARATE PARENT STEP. There was one, and it RETURNED when the parent's
    # diff was not release-only -- so a docs-only push, whose parent diff is
    # prose, never reached the walk that exists for exactly that case. The walk
    # starts at $want itself and applies the more general test at every step,
    # so the parent is simply its second iteration.
    why=$(_release_gate_walk_back "$want" _release_gate_ci_green \
              "full CI run (with '$AXL_CI_SUITE_JOB')")
    rc=$?
    printf '%s\n' "$why"
    # The commonest refusal since §14, and the fix is one command: say it.
    [[ $rc -eq 1 ]] && echo "the suite runs only on workflow_dispatch: gh workflow run ci.yml --ref main, then wait for it"
    return "$rc"
}

# _release_gate_walk_back <want> <checker> <what> — the bounded walk, shared
# by both halves: the first ancestor of <want> (itself included) whose diff to
# <want> is inert AND which <checker> accepts. Prints which ancestor answered,
# or every examined step's reason. Returns the checker's verdict: 0, 1, or 2
# when any step could not be checked and none was accepted -- an unanswered
# question near the tip must not read as "red" further down.
_release_gate_walk_back() {
    local want=$1 checker=$2 what=$3 n=0 c why rc could_not=0 last_rc=1
    local lines=()
    while read -r c; do
        [[ -n "$c" ]] || continue
        n=$((n + 1))
        [[ "$n" -le "$AXL_CI_WALK_MAX" ]] || break
        release_gate_diff_inert "$c" "$want" || continue
        why=$("$checker" "$c")
        rc=$?
        if [[ $rc -eq 0 ]]; then
            if [[ "$c" == "$want" ]]; then
                echo "$what is green on $c"
            else
                echo "$what is green on $c, and everything since is prose or the version bump"
            fi
            return 0
        fi
        [[ $rc -eq 2 ]] && could_not=1
        last_rc=$rc
        lines+=("  ${c:0:8}: $why")
    done < <(git rev-list --max-count="$AXL_CI_WALK_MAX" "$want" 2>/dev/null)
    if [[ $could_not -eq 1 ]]; then
        echo "could not check $what for $want:"
        printf '%s\n' "${lines[@]}"
        return 2
    fi
    # STILL RUNNING only when the run that DECIDES is: the last commit the
    # walk examined, the oldest one whose diff to <want> is still inert. A
    # docs tip's own run still going decides nothing -- its cheap jobs will
    # skip -- so it does not make the answer "pending".
    if [[ $last_rc -eq 3 ]]; then
        echo "the $what deciding $want is still running:"
        printf '%s\n' "${lines[@]}"
        return 3
    fi
    echo "no green $what at $want or an inert ancestor within $AXL_CI_WALK_MAX commits:"
    [[ ${#lines[@]} -gt 0 ]] && printf '%s\n' "${lines[@]}"
    return 1
}

# ── the decision cut-release.sh enforces (review ruling R-B) ─────────────

# release_gate_decide <sha> <ci_gate> <tag_untested> [branch] -- THE gate, for
# the normal flow and --resume alike. Sets RELEASE_GATE_DECISION to one of
# covered-local, covered-ci, dispatch, untested, refuse, prints why, and
# returns 1 only for refuse.
#
# ENFORCED BY DEFAULT. A cut with nothing certifying the commit used to warn
# and tag anyway; a public tag cannot be re-cut, so a warning nobody reads is
# not a gate. Proceeding takes a flag: --ci-gate (dispatch the suite, wait for
# it) or --tag-untested (loud, and recorded in the release notes).
# shellcheck disable=SC2034  # RELEASE_GATE_DECISION is the result; callers read it
release_gate_decide() {
    local sha=$1 ci_gate=$2 untested=$3 branch=${4:-main} lw cw
    RELEASE_GATE_DECISION=""
    if lw=$(release_gate_covers "$sha"); then
        RELEASE_GATE_DECISION=covered-local
        printf 'covered by the local release suite:\n%s\n' "$lw"
        return 0
    fi
    if cw=$(release_gate_ci_covers "$sha"); then
        RELEASE_GATE_DECISION=covered-ci
        printf 'covered by CI:\n%s\n' "$cw"
        return 0
    fi
    printf 'nothing certifies %s:\n' "$sha"
    printf '  local release suite: %s\n' "$(sed '2,$s/^/    /' <<< "$lw")"
    printf '  CI:                  %s\n' "$(sed '2,$s/^/    /' <<< "$cw")"
    cat <<EOF
either of these certifies it:
  1. dispatch the suite and wait for it to go green:
       gh workflow run ci.yml --ref $branch
     (cut-release.sh --ci-gate does exactly that)
  2. run the release suite locally on a clean tree at this commit, with the
     cheap push CI ($(_release_gate_join ', ' "${AXL_CI_CHEAP_JOBS[@]}")) green on it:
       ./scripts/release-suite.sh
EOF
    if [[ "$ci_gate" == true ]]; then
        RELEASE_GATE_DECISION=dispatch
        echo "--ci-gate: dispatching the suite on $branch and waiting for it"
        return 0
    fi
    if [[ "$untested" == true ]]; then
        RELEASE_GATE_DECISION=untested
        echo "WARNING: --tag-untested: tagging $sha with NOTHING certifying it; this is recorded in the release notes"
        return 0
    fi
    RELEASE_GATE_DECISION=refuse
    echo "refusing to tag: --ci-gate dispatches the suite and waits; --tag-untested overrides, and is recorded in the release notes"
    return 1
}

# release_gate_untested_note <sha> -- the line --tag-untested writes into the
# release's CHANGELOG section (which release.yml publishes as the notes) and
# the tag message. One spelling, so the two cannot say different things.
release_gate_untested_note() {
    printf '**Released untested (--tag-untested).** Neither a dispatched CI suite run nor a local scripts/release-suite.sh run certified %s when this tag was cut.\n' "$1"
}

# release_gate_record_untested <changelog> <heading> <note> -- put <note>
# directly under <heading> (the just-dated "## X.Y.Z — DATE" line), which is
# the section release.yml publishes as the release notes
# (scripts/changelog-section.sh). Returns 1 when the note did not land -- a
# heading that is not there -- so the caller can refuse rather than ship an
# untested release whose notes do not say so.
release_gate_record_untested() {
    local f=$1 heading=$2 note=$3
    awk -v h="$heading" -v n="$note" \
        '{print} $0==h && !done {print ""; print n; done=1}' "$f" > "$f.tmp" \
        && mv "$f.tmp" "$f" || { rm -f "$f.tmp"; return 1; }
    grep -qxF -- "$note" "$f"
}

# release_gate_wait_push_run <sha> -- wait for the PUSH run at <sha> to
# finish; 0 when it succeeded (or there is none), 1 when it did not, 2 when gh
# could not answer, 3 when it is still running at the limit.
#
# WHY (review ruling R-C). cut-release pushes the [release-cut] commit on top
# of <sha>, and a push run still going below it could be cancelled by that
# push or simply not have answered yet -- so the cut waits for it first, and
# refuses (fails closed) when it did not succeed. NO RUN AT ALL is not a
# failure: an unpushed HEAD has none, and the gate then decides without one.
AXL_GATE_POLL_SECS="${AXL_GATE_POLL_SECS:-30}"
AXL_GATE_POLL_MAX="${AXL_GATE_POLL_MAX:-90}"
release_gate_wait_push_run() {
    local sha=$1 i=0 gh="${AXL_GH:-gh}" row h st cc did
    command -v "$gh" >/dev/null 2>&1 || {
        echo "could not check the push run at $sha: gh is not available ($gh not found)"; return 2; }
    while :; do
        i=$((i + 1))
        row=$(_release_gate_gh run list --workflow=ci.yml --commit "$sha" --event push \
                  --limit 1 --json headSha,status,conclusion,databaseId \
                  --jq '.[] | [.headSha, .status, .conclusion, .databaseId] | @tsv' \
                  2>/dev/null) || {
            echo "could not check the push run at $sha: \`gh run list\` failed"; return 2; }
        if [[ -z "${row//[[:space:]]/}" ]]; then
            echo "no push run at $sha (not pushed, or the event was dropped) -- the gate decides without one"
            return 0
        fi
        IFS=$'\t' read -r h st cc did <<< "$row"
        if [[ "$st" == "completed" ]]; then
            if [[ "$cc" == "success" ]]; then
                echo "push run $did at $sha: success"
                return 0
            fi
            echo "push run $did at $sha concluded $cc -- refusing; fix it and push again"
            return 1
        fi
        if [[ $i -ge $AXL_GATE_POLL_MAX ]]; then
            echo "push run $did at $sha is still $st after $i polls -- refusing"
            return 3
        fi
        [[ $i -eq 1 ]] && echo "waiting for push run $did at $sha ($st) ..." >&2
        sleep "$AXL_GATE_POLL_SECS"
    done
}
