#!/bin/bash
# cut-release.sh — one-command release cut, per docs/RELEASING.md.
#
# Automates the whole flow, and ENFORCES the release gate before it changes
# anything (docs/RELEASING.md "The gate"; AXL-CI-Release-Speed-Design.md §14).
# The commit is certified by EITHER a green dispatched CI suite run on it (or
# its release-only parent), OR a complete local scripts/release-suite.sh run
# on it plus a green push run of the cheap CI jobs. With neither, the cut
# REFUSES -- unless --ci-gate (dispatch the suite after pushing the release
# commit, and wait for it) or --tag-untested (tag anyway, loudly, with a
# "Released untested" line in the release notes and the tag message). A
# public tag can't be cleanly re-cut once Release publishes.
#
# Usage:
#   scripts/cut-release.sh X.Y.Z            # cut a release; refuses if nothing certifies it
#   scripts/cut-release.sh X.Y.Z --dry-run  # show what it would do (and whether it
#                                           #   would refuse), change nothing
#   scripts/cut-release.sh X.Y.Z --yes      # skip the confirmation prompt
#   scripts/cut-release.sh X.Y.Z --ci-gate  # nothing covers it yet: dispatch the
#                                           #   suite and wait for it before tagging
#   scripts/cut-release.sh X.Y.Z --force-ci # --ci-gate, even if something covers it
#   scripts/cut-release.sh X.Y.Z --tag-untested  # tag with NOTHING certifying it;
#                                           #   recorded in the notes. Not with --resume
#   scripts/cut-release.sh X.Y.Z --allow-breaking  # cut a documented breaking
#                                           #   change under a non-major version
#   scripts/cut-release.sh X.Y.Z --watch    # wait for Release+Docs instead of
#                                           #   being handed a URL
#   scripts/cut-release.sh X.Y.Z --resume   # recovery: the release branch already
#                                           #   has the release commit -- the same
#                                           #   gate, then tag, push, watch
#   scripts/cut-release.sh X.Y.Z --branch release-X.Y.Z
#                                           # cut from a tag on a release branch
#                                           #   (RELEASING.md "Cutting from a
#                                           #   tag"): ships a fix to released
#                                           #   users without whatever main has
#                                           #   picked up since. Also accepts a
#                                           #   maintenance line, release/X.Y.x
#
# What it does (normal flow):
#   1. preconditions  : on the release branch (main by default), clean tree,
#                       gh authed, tag absent, CHANGELOG has an "## Unreleased"
#                       section, semver matches the CHANGELOG
#   2. GATE           : wait for HEAD's push run to finish (refuse if it failed),
#                       then decide: covered, --ci-gate, --tag-untested, or
#                       REFUSE -- all BEFORE the bump, so a refusal strands no
#                       release commit
#   3. docs gate      : build-docs.sh --ci-doxygen (unless --no-docs-check)
#   4. bump-version   : VERSION + axl-version.h (via scripts/bump-version.sh)
#   5. date CHANGELOG : "## Unreleased" -> "## X.Y.Z — YYYY-MM-DD" (+ the
#                       --tag-untested note under it, or the cut dies)
#   6. check-version  : scripts/check-version.sh
#   7. commit + push  : one "release: vX.Y.Z" commit -> origin/<release branch>
#   8. dispatch       : only when step 2 decided it: dispatch ci.yml, wait GREEN
#   9. tag + push     : annotated vX.Y.Z (body summarised from the CHANGELOG)
#  10. watch + confirm: scripts/watch-release-runs.sh, then gh release view
#
# It does NOT run the heavy local suite itself. To certify locally, run
# scripts/release-suite.sh on the commit first (CI's integration job, the same
# steps; it writes the stamp step 2 reads) -- or pass --ci-gate and let CI.

set -euo pipefail

# --------------------------------------------------------------------------
# args
# --------------------------------------------------------------------------
VERSION=""
DRY_RUN=false
ASSUME_YES=false
RESUME=false
FORCE_CI=false  # --force-ci: dispatch CI even when the local stamp
                # already covers the release commit.
TAG_UNTESTED=false  # --tag-untested: tag with NOTHING certifying the commit.
                    # Loud, and written into the release notes -- the gate
                    # otherwise refuses (review ruling R-B).
# The release gate: the stamp reader (scripts/release-suite.sh is its only
# writer), the CI queries and release_gate_decide. Sourced, not re-implemented.
# shellcheck source=../test/integration/lib/release-gate.sh
source "$(dirname "$0")/../test/integration/lib/release-gate.sh"
# axl_trap_cleanup / axl_die_by_signal -- a signal ENDS this script after its
# cleanup, rather than resuming it. See where they are used below.
# shellcheck source=axl-common.sh
source "$(dirname "$0")/axl-common.sh"

CI_GATE=false   # A push to main runs only ci.yml's CHEAP jobs; the QEMU suite
                # runs on workflow_dispatch alone (AXL-CI-Release-Speed-Design
                # §14). So a release is certified by a dispatched suite run, or
                # by a local uncached run plus the cheap push CI -- see the gate
                # below. --ci-gate makes this script dispatch ci.yml on the
                # release branch and wait for it when nothing covers the commit
                # yet. A --branch cut turns it on itself: a push to a non-main
                # branch starts no run at all.
RELEASES_REPO="aximcode/axl-sdk-releases"
# Deliberate exception to the semver refusal below: a release that really is
# meant to carry a documented breaking change under a non-major version.
ALLOW_BREAKING=""
# The pre-tag docs gate is ON by default; see where it runs for why.
RUN_DOCS_CHECK=true
# Watching Release+Docs to completion is now OPT-IN. It cost 4-7 minutes of a
# human's attention per release and returned a workflow exit status;
# check-published-release.sh verifies the published bytes instead, which is
# both stronger and asynchronous. --watch restores the old behaviour.
WATCH=false
# The branch this release is cut from. `main` is the line; a maintenance line
# (`release/X.Y.x`) cuts a patch that deliberately excludes what landed on main
# afterwards -- a fix for released users when main is mid-feature.
RELEASE_BRANCH="main"

# while/shift rather than `for arg`: --branch takes a value. Both spellings are
# accepted so neither habit is wrong.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=true ;;
        --yes|-y)  ASSUME_YES=true ;;
        --resume)  RESUME=true ;;
        --ci-gate) CI_GATE=true ;;
        --force-ci) CI_GATE=true; FORCE_CI=true ;;
        --tag-untested) TAG_UNTESTED=true ;;
        --allow-breaking) ALLOW_BREAKING=1 ;;
        --no-docs-check)  RUN_DOCS_CHECK=false ;;
        --watch)          WATCH=true ;;
        --branch)
            [[ $# -ge 2 ]] || { echo "ERROR: --branch needs a branch name" >&2; exit 2; }
            RELEASE_BRANCH="$2"; shift
            ;;
        --branch=*) RELEASE_BRANCH="${1#--branch=}" ;;
        -*)        echo "ERROR: unknown flag '$1'" >&2; exit 2 ;;
        *)
            if [[ -n "$VERSION" ]]; then
                echo "ERROR: version given twice ('$VERSION', '$1')" >&2; exit 2
            fi
            VERSION="$1"
            ;;
    esac
    shift
done

# --tag-untested CANNOT ride a --resume: the release commit is already pushed,
# and release.yml builds the notes from its CHANGELOG section alone, so the
# note would reach the tag message and not the notes a consumer reads.
# Checked before anything else about the arguments, so it is the answer.
if $RESUME && $TAG_UNTESTED; then
    echo "ERROR: --tag-untested cannot be used with --resume: the release commit is already pushed, and release.yml builds the release notes from its CHANGELOG section alone, so the note could not reach them." >&2
    echo "       Certify the commit instead: --resume --ci-gate dispatches the suite and waits; or run scripts/release-suite.sh on it, then --resume." >&2
    exit 2
fi

if [[ -z "$VERSION" ]]; then
    echo "usage: $0 X.Y.Z [--branch release-X.Y.Z|release/X.Y.x] [--dry-run] [--yes]" \
         "[--resume] [--ci-gate] [--force-ci] [--tag-untested] [--watch] [--allow-breaking]" \
         "[--no-docs-check]" >&2
    exit 2
fi
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: version must be X.Y.Z (got '$VERSION')" >&2
    exit 2
fi

TAG="v$VERSION"
# Only a non-default branch belongs in the resume hint the recovery text prints.
RESUME_HINT=""
[[ "$RELEASE_BRANCH" == "main" ]] || RESUME_HINT=" --branch $RELEASE_BRANCH"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The branch name must ENCODE the version being cut, so `--branch` cannot become
# "release from wherever I happen to be". Two shapes, both already in use:
# `release-X.Y.Z` is the per-release branch RELEASING.md's "Cutting from a tag"
# section documents (v3.2.1, v3.2.2 and v3.2.3 were all cut this way);
# `release/X.Y.x` is a longer-lived maintenance line that carries successive
# patches of one minor.
if [[ "$RELEASE_BRANCH" != "main" ]]; then
    if [[ "$RELEASE_BRANCH" =~ ^release-([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
        [[ "${BASH_REMATCH[1]}" == "$VERSION" ]] \
            || { echo "ERROR: branch $RELEASE_BRANCH does not name the version being cut ($VERSION)" >&2; exit 2; }
    elif [[ "$RELEASE_BRANCH" =~ ^release/([0-9]+)\.([0-9]+)\.x$ ]]; then
        LINE_MAJOR="${BASH_REMATCH[1]}"; LINE_MINOR="${BASH_REMATCH[2]}"
        IFS=. read -r V_MAJOR V_MINOR V_PATCH <<<"$VERSION"
        if [[ "$V_MAJOR" != "$LINE_MAJOR" || "$V_MINOR" != "$LINE_MINOR" \
              || "$V_PATCH" == "0" ]]; then
            echo "ERROR: $RELEASE_BRANCH may only cut patches of ${LINE_MAJOR}.${LINE_MINOR}" \
                 "(got $VERSION). A new minor or major comes from main." >&2
            exit 2
        fi
    else
        echo "ERROR: --branch must be main, release-X.Y.Z or release/X.Y.x (got '$RELEASE_BRANCH')" >&2
        exit 2
    fi
    # NOT OPTIONAL HERE, and this is the one thing that makes a maintenance cut
    # different in kind: ci.yml triggers on `push: branches: [main]`, so pushing
    # this branch starts NO run and the gate below would wait 75 minutes for a
    # run that can never appear. Dispatch is the only way CI ever sees this ref.
    # (A dispatched run also ignores the [release-cut] marker: that guard reads
    # github.event.head_commit, which is null on workflow_dispatch.)
    CI_GATE=true
fi
cd "$REPO_ROOT"

say()  { printf '\n=== %s ===\n' "$*"; }
note() { printf '  %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------------------
# preconditions (shared)
# --------------------------------------------------------------------------
say "Preconditions for $TAG"
command -v gh >/dev/null   || die "gh (GitHub CLI) not found"
# Ask the API, not `gh auth status`. Status reports on EVERY configured
# account and exits non-zero if ANY of them has a bad token — so one stale
# login left over from another identity blocks a release while every gh call
# this script makes would have worked fine against the active account. A
# `gh api user` round-trip tests exactly what we depend on: that the active
# account can reach the API.
gh api user -q .login >/dev/null 2>&1 \
    || die "gh cannot reach the API as the active account (gh auth login)"

[[ "$(git branch --show-current)" == "$RELEASE_BRANCH" ]] \
    || die "not on branch $RELEASE_BRANCH (use --branch to cut a maintenance line)"

# Working tree must be clean (gitignored files like CLAUDE.md / .claude are fine).
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    git status --short
    die "working tree has uncommitted tracked changes"
fi

git tag -l "$TAG" | grep -q . && die "tag $TAG already exists locally"
if git ls-remote --tags origin "$TAG" 2>/dev/null | grep -q .; then
    die "tag $TAG already exists on origin"
fi
note "on $RELEASE_BRANCH, clean tree, $TAG is free, gh authenticated"

# --------------------------------------------------------------------------
# wait_for_ci <sha> — poll the CI workflow for <sha> until it completes.
#   returns 0 on success, 1 on failure/timeout.
# --------------------------------------------------------------------------
wait_for_ci() {
    local sha="$1" i line
    # 150 * 30s = 75 min. The old ceiling was 30 min, chosen when CI's QEMU job
    # ran a handful of suites; it now runs the WHOLE integration set (145 tests,
    # each in its own QEMU) on a 2-core runner, which measured ~50 min on the
    # v3.2.0 cut — so the gate timed out on a run that went on to pass, and the
    # release had to be finished with --resume. Size the ceiling to the job.
    say "Waiting for CI to pass on $sha (the release gate; up to 75 min)"
    local why
    for i in $(seq 1 150); do
        # DISPATCHED RUNS ONLY. Every caller of this has just dispatched (or is
        # resuming after one), and since §14 the suite runs on nothing else.
        # Without the filter the newest run at the sha could be its PUSH run --
        # the release commit's, which skips every job -- if the dispatch had
        # not registered yet, and that answered "not green" and aborted the cut.
        line="$(gh run list --commit "$sha" --workflow CI --event workflow_dispatch \
                  --json status,conclusion \
                  --jq '.[0] | "\(.status):\(.conclusion)"' 2>/dev/null || true)"
        case "$line" in
            completed:success)
                # RUN-LEVEL SUCCESS IS NOT ENOUGH. GitHub reports an all-skipped
                # run as success, and the `[release-cut]` marker makes exactly
                # that shape reachable — "tag code no suite ever ran". The
                # gate helper asserts the suite job AND the three cheap jobs
                # succeeded, by name; use it rather than a weaker second
                # opinion. On a release branch this is the only gate.
                if why=$(_release_gate_ci_green "$sha"); then note "CI: SUCCESS"; return 0; fi
                # The helper says WHY -- a job that did not run, one that did
                # not pass, or a listing it could not read -- so print that,
                # and hand over the run so a human can settle it in one click.
                note "CI reported success, but it does not certify $sha: $why"
                note "Refusing — check the run:"
                note "    $(gh run list --commit "$sha" --workflow CI --event workflow_dispatch \
                              --limit 1 --json url --jq '.[0].url' 2>/dev/null \
                          || echo "gh run list --commit $sha --workflow CI")"
                return 1 ;;
            completed:*)       note "CI: ${line#completed:}"; return 1 ;;
            *)
                # One line per 5 min, not per poll: 150 identical
                # "in_progress" lines bury the outcome they precede.
                if (( i == 1 || i % 10 == 0 )); then
                    note "[$(( (i * 30) / 60 )) min] CI: ${line:-no run yet}"
                fi
                ;;
        esac
        sleep 30
    done
    note "timed out waiting for CI after 75 min (the run may still be going —"
    note "check 'gh run list --workflow CI', then use --resume)"
    return 1
}

# --------------------------------------------------------------------------
# make_tag_message — annotated-tag body summarised from the CHANGELOG section.
# --------------------------------------------------------------------------
make_tag_message() {
    local prev
    prev="$(git describe --tags --abbrev=0 2>/dev/null || true)"
    {
        echo "$TAG"
        echo
        echo "Highlights:"
        echo
        # Bold entry titles from the just-dated "## X.Y.Z" CHANGELOG section.
        awk -v ver="## $VERSION " '
            $0 ~ "^" ver {f=1; next}
            f && /^## / {f=0}
            f && /^- \*\*/ {
                line=$0
                sub(/^- \*\*/, "", line); sub(/\*\*.*/, "", line)
                print "  - " line
            }' CHANGELOG.md | head -12
        echo
        echo "See CHANGELOG.md for the full list${prev:+ (changes since $prev)}."
        [[ -z "${UNTESTED_NOTE:-}" ]] || { echo; echo "$UNTESTED_NOTE"; }
    }
}

# --------------------------------------------------------------------------
# tag_and_publish <sha> — tag, push, watch, confirm. Assumes CI already green.
# --------------------------------------------------------------------------
tag_and_publish() {
    local sha="$1"
    say "Tagging $TAG at $sha"
    if $DRY_RUN; then
        note "DRY RUN — would: git tag -a $TAG && git push origin $TAG"
        if $WATCH; then
            note "DRY RUN — would: scripts/watch-release-runs.sh $TAG"
            note "DRY RUN — would: gh release view $TAG --repo $RELEASES_REPO"
        else
            note "DRY RUN — would print the Actions URL and NOT wait (--watch to wait)"
            note "DRY RUN — would suggest: scripts/check-published-release.sh $TAG"
        fi
        return 0
    fi
    git tag -a "$TAG" -m "$(make_tag_message)"
    git push origin "$TAG"

    # EVERY `v*` tag starts BOTH Release and Docs -- docs.yml's trigger is
    # `push: tags: ['v*']`, with no version filter, and RELEASING.md says so in
    # as many words. This used to expect Docs only on a MAJOR tag (vX.0.0),
    # which meant that on a minor or patch the watcher never WAITED for it: if
    # Docs was still queued when Release finished, the cut reported success
    # having never looked at it. v4.3.0 only got checked because an unrelated CI
    # run kept the poll loop alive. A tag cannot be re-cut, so a Docs failure
    # nobody waited for is a broken docs site with a green release.
    #
    # CI is not triggered by the TAG. On main it IS triggered by the `git push
    # origin main` this script does two steps earlier, which carries the release
    # commit, so a CI run on this same SHA is expected and the watcher will see
    # it -- it just is not something the tag started. On a release branch no
    # push-triggered run exists at all; the one the gate used was dispatched.
    local expect="Release Docs"
    if ! $WATCH; then
        # HANDING BACK A URL BEATS HOLDING A HUMAN. Watching to completion cost
        # 4-7 minutes of somebody's attention per release, and what it bought
        # was a workflow's exit status -- while check-published-release.sh
        # verifies the published BYTES, which is the stronger claim and the one
        # that actually catches a truncated upload or a missed rename (§16.2
        # job 2). The tag is pushed either way; nothing here can un-publish it.
        say "$TAG pushed. Release + Docs are running:"
        note "    $(gh repo view --json url --jq .url 2>/dev/null || echo https://github.com/aximcode/axl-sdk)/actions"
        note ""
        note "When they finish, verify what consumers will actually receive:"
        note "    scripts/check-published-release.sh $TAG"
        note ""
        note "To watch instead of being handed the link, re-run with --watch."
        return 0
    fi

    say "Watching $expect for $TAG"
    if ! EXPECT_WORKFLOWS="$expect" scripts/watch-release-runs.sh "$TAG"; then
        die "a release workflow did not succeed — see the output above and 'gh run list'"
    fi

    say "Published release"
    gh release view "$TAG" --repo "$RELEASES_REPO" \
        --json name,tagName,isDraft,assets \
        --jq '.name, .tagName, ("draft=" + (.isDraft|tostring)), ("assets=" + (.assets|length|tostring))'
    say "$TAG is live on $RELEASES_REPO. Done."
    # The bump changed include/axl/axl-version.h, so the staged SDK under
    # ./stage is now stale BY CONSTRUCTION -- every cut does this. Say it here,
    # because otherwise the next `run-integration.sh` discovers it ~30 s in,
    # refuses, and costs a round trip that nobody chose.
    note ""
    note "NOTE: the version bump makes the staged SDK stale. Before the next"
    note "      suite run:  ./scripts/install.sh --arch all --cpp"
}

# --------------------------------------------------------------------------
# --resume: the release branch already carries the release commit. With the
# gate on, a green dispatched suite run covers it, or this dispatches one and
# waits -- on main too, since a push runs no suite (§14). Then tag.
# --------------------------------------------------------------------------
if $RESUME; then
    [[ "$(cat VERSION)" == "$VERSION" ]] || die "VERSION is $(cat VERSION), not $VERSION — nothing to resume"
    [[ "$(git rev-parse HEAD)" == "$(git rev-parse "origin/$RELEASE_BRANCH" 2>/dev/null)" ]] \
        || die "HEAD != origin/$RELEASE_BRANCH — push $RELEASE_BRANCH first"
    RESUME_SHA="$(git rev-parse HEAD)"
    UNTESTED_NOTE=""
    # THE SAME GATE AS THE NORMAL FLOW (review ruling R-B): covered, or
    # --ci-gate dispatches and waits, or it refuses. (--tag-untested is
    # refused with --resume at argument parsing -- see there.) A resume used
    # to tag without asking unless --ci-gate was given.
    if $FORCE_CI; then
        RELEASE_GATE_DECISION=dispatch
    else
        say "Release gate"
        release_gate_decide "$RESUME_SHA" "$CI_GATE" "$TAG_UNTESTED" "$RELEASE_BRANCH" || true
        [[ "$RELEASE_GATE_DECISION" != refuse ]] || exit 1
    fi
    case "$RELEASE_GATE_DECISION" in
        dispatch)
            # DISPATCH ON EVERY BRANCH, main included: a push to a non-main
            # branch starts no ci.yml run at all, and since §14 a push to main
            # starts one WITHOUT the suite. A dispatched run STILL GOING -- the
            # post-timeout resume the 75-min ceiling exists for -- is waited
            # on, not replaced: dispatches share a concurrency group with
            # cancel-in-progress, so a second dispatch would cancel it.
            _st="$(gh run list --commit "$RESUME_SHA" --workflow CI --event workflow_dispatch \
                     --json status --jq '.[0].status // ""' 2>/dev/null || true)"
            case "$_st" in
                queued|in_progress|waiting|requested|pending)
                    note "a dispatched CI run at $RESUME_SHA is $_st — waiting on it" ;;
                *)
                    say "Dispatching CI on $RELEASE_BRANCH (a push runs no suite)"
                    gh workflow run ci.yml --ref "$RELEASE_BRANCH" \
                        || die "could not dispatch ci.yml (gh auth / workflow name?)"
                    sleep 10   # let the dispatched run register before polling
                    ;;
            esac
            wait_for_ci "$RESUME_SHA" || die "CI is not green on origin/$RELEASE_BRANCH HEAD"
            ;;
    esac
    tag_and_publish "$RESUME_SHA"
    exit 0
fi

# --------------------------------------------------------------------------
# normal flow
# --------------------------------------------------------------------------
grep -q '^## Unreleased' CHANGELOG.md \
    || die "CHANGELOG.md has no '## Unreleased' section — add release notes first"

# ...and that the section is consistent with the version being cut. The check
# above only asks whether the heading EXISTS; step 3 then dates it over
# whatever sits beneath. That is how v3.2.3 first shipped 43 commits of
# in-progress work, two of them "### Breaking", as a PATCH. The commit range is
# already printed below and it did not help -- and --yes skips the prompt
# entirely -- so this is a refusal rather than more output.
scripts/check-release-semver.sh "$VERSION" \
    ${ALLOW_BREAKING:+--allow-breaking} \
    || die "release content does not match the version being cut"

PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || true)"

# WHAT THE BRANCH CONTAINS, not just what it is called. The name guard above
# stops a release branch being pushed onto main; it cannot stop a branch that
# was cut from main instead of from the tag -- `git checkout -b release-X.Y.Z`
# one directory up, and every other check still passes while 70-odd commits of
# mid-feature work ship as a patch. That is the v3.2.3 incident, which the
# commit range printed below did not prevent (and --yes skips the prompt
# entirely). A cut-from-tag release is a handful of cherry-picks; anything
# larger is refused by default and has to say so out loud.
MAX_BRANCH_COMMITS="${AXL_MAX_BRANCH_COMMITS:-10}"
[[ "$MAX_BRANCH_COMMITS" =~ ^[0-9]+$ ]] \
    || die "AXL_MAX_BRANCH_COMMITS must be a number (got '$MAX_BRANCH_COMMITS')"
if [[ "$RELEASE_BRANCH" != "main" && -n "$PREV_TAG" ]]; then
    BRANCH_COMMITS="$(git rev-list --count "$PREV_TAG..HEAD")"
    (( BRANCH_COMMITS <= MAX_BRANCH_COMMITS )) || die \
"$RELEASE_BRANCH carries $BRANCH_COMMITS commits since $PREV_TAG — that looks
       like main, not a cut-from-tag release. Check you branched from the TAG.
       If it really is deliberate:  AXL_MAX_BRANCH_COMMITS=$BRANCH_COMMITS $0 $VERSION --branch $RELEASE_BRANCH"
fi

# ── THE RELEASE GATE, decided BEFORE anything changes (review ruling R-B) ──
#
# ENFORCED BY DEFAULT. A release is certified by a green DISPATCHED CI suite
# run on this tree, or by a local scripts/release-suite.sh stamp on it plus a
# green push run of the cheap jobs (AXL-CI-Release-Speed-Design.md §14). With
# neither, this REFUSES -- here, before the bump, so a refusal strands no
# release commit on $RELEASE_BRANCH. Proceeding without one takes a flag:
# --ci-gate (dispatch the suite after pushing the release commit, and wait) or
# --tag-untested (loud, and recorded in the release notes).
#
# FIRST, WAIT FOR HEAD'S PUSH RUN (review ruling R-C). Its cheap jobs are half
# of the local path, and the [release-cut] push about to land on top of it
# must not race it. A run that did not succeed refuses the cut.
if $DRY_RUN; then
    note "DRY RUN — would wait for the push run at $(git rev-parse HEAD) to finish"
elif ! WAIT_WHY="$(release_gate_wait_push_run "$(git rev-parse HEAD)")"; then
    die "$WAIT_WHY"
else
    note "$WAIT_WHY"
fi
UNTESTED_NOTE=""
if $FORCE_CI; then
    RELEASE_GATE_DECISION=dispatch
    note "--force-ci: dispatching the suite whatever covers the commit"
else
    say "Release gate"
    release_gate_decide "$(git rev-parse HEAD)" "$CI_GATE" "$TAG_UNTESTED" "$RELEASE_BRANCH" || true
    if $DRY_RUN; then
        [[ "$RELEASE_GATE_DECISION" != refuse ]] || note "DRY RUN — a real cut REFUSES here"
    else
        [[ "$RELEASE_GATE_DECISION" != refuse ]] || exit 1
    fi
fi
[[ "$RELEASE_GATE_DECISION" != untested ]] \
    || UNTESTED_NOTE="$(release_gate_untested_note "$(git rev-parse HEAD)")"

# THE DOCS GATE, and it runs BEFORE anything is pushed because it is the one
# gate that cannot be re-run after the fact: docs.yml fires on every `v*` tag
# and a tag cannot be re-cut. A dev box's doxygen is NEWER than the apt package
# CI installs and ACCEPTS markup the older one rejects, so a locally-clean
# `verify.sh` is not evidence -- that skew shipped broken docs at v3.2.0 and
# again at v4.2.0. RELEASING.md called it "the one gate worth running that the
# normal verify.sh cannot substitute for"; being documented was not enough, so
# it is enforced here.
#
# Escapable by name (--no-docs-check) rather than absent: a check with no way
# past it gets commented out the first time it is inconvenient. The flag says
# out loud what was skipped.
if $RUN_DOCS_CHECK; then
    say "Docs gate — rebuilding at CI's doxygen version (this is the pre-tag gate)"
    scripts/build-docs.sh --ci-doxygen \
        || die "docs are not clean at CI's doxygen version — docs.yml would fail on the tag, and a tag cannot be re-cut. Fix the markup, or re-run with --no-docs-check if you accept a broken docs deploy."
else
    note "docs gate SKIPPED (--no-docs-check) — docs.yml still fires on this tag"
fi

say "Will release $TAG — commits since ${PREV_TAG:-the beginning}"
git --no-pager log --oneline "${PREV_TAG:+$PREV_TAG..}HEAD" | sed 's/^/  /'

if ! $ASSUME_YES && ! $DRY_RUN; then
    # Refuse up front when there is nobody to ask. Without this the `read`
    # below hits EOF, `reply` stays empty, and the run dies with "aborted by
    # user" -- naming a user who was never prompted, on a terminal that never
    # existed. A caller then has to distinguish "someone declined" from "this
    # cannot prompt", and the two need different fixes.
    if [[ ! -t 0 ]]; then
        die "stdin is not a terminal, so the confirmation prompt cannot be
       answered. Nothing was pushed and no tag was created. Re-run with
       --yes to cut without prompting:

           scripts/cut-release.sh ${VERSION} --yes"
    fi
    printf '\nCut and publish %s? This pushes %s and creates a public tag. [y/N] ' "$TAG" "$RELEASE_BRANCH"
    read -r reply
    [[ "$reply" == "y" || "$reply" == "Y" ]] \
        || die "declined at the prompt. Nothing was pushed and no tag was created."
fi

say "Bumping version + dating CHANGELOG"
# From here until the release-metadata commit lands, the working tree carries an
# UNCOMMITTED version bump. Restore it on any early exit — a SIGPIPE from a
# truncated pager (`cut-release.sh X.Y.Z --dry-run | head`), a Ctrl-C, or any
# `set -e` failure — so a half-finished run can't strand a stray bump that makes
# the next cut abort on the clean-tree precondition. (The happy-path revert
# below is not enough on its own: with `set -euo pipefail`, a SIGPIPE'd
# `git diff` kills the script before it is reached.)
#
# INT/TERM/HUP go through axl_trap_cleanup (axl-common.sh), not a bare
# `trap restore_metadata EXIT INT TERM HUP`: a trap handler that does not
# itself exit RESUMES the script where the signal landed, at the interrupted
# command's own failure status -- so a Ctrl-C here restored the metadata and
# then carried on into whatever came next, with `set -e` catching it only
# sometimes (never in a condition context). axl_die_by_signal runs
# restore_metadata itself, once, and then dies by the signal.
restore_metadata() {
    git checkout -- VERSION include/axl/axl-version.h CHANGELOG.md 2>/dev/null || true
}
axl_trap_cleanup restore_metadata INT TERM HUP

scripts/bump-version.sh "$VERSION" >/dev/null
TODAY="$(date +%Y-%m-%d)"
# Date only the FIRST "## Unreleased" (the active section).
sed -i.bak "0,/^## Unreleased$/s//## $VERSION — $TODAY/" CHANGELOG.md
rm -f CHANGELOG.md.bak
# --tag-untested goes INTO the release notes: release.yml publishes this
# section as the GitHub release body, so a consumer reading it sees it.
if [[ -n "$UNTESTED_NOTE" ]]; then
    release_gate_record_untested CHANGELOG.md "## $VERSION — $TODAY" "$UNTESTED_NOTE" \
        || die "--tag-untested: the note did not land in the '## $VERSION — $TODAY' section, so the release notes would not say this is untested -- refusing"
    note "recorded --tag-untested in the CHANGELOG section"
fi
note "VERSION -> $VERSION ; CHANGELOG '## Unreleased' -> '## $VERSION — $TODAY'"

# A refusal, not a note: `cmd && note` under errexit SKIPS the note and goes
# on to commit when cmd fails (errexit ignores a non-final command of a &&
# list) -- the check could never stop a cut.
"$REPO_ROOT/scripts/check-version.sh" || die "VERSION and axl-version.h disagree (above)"
note "check-version OK"

if $DRY_RUN; then
    say "DRY RUN — release-metadata diff that WOULD be committed"
    git --no-pager diff -- VERSION include/axl/axl-version.h CHANGELOG.md
    note "DRY RUN — would: git commit -m 'release: $TAG' (+ [release-cut]) && git push origin $RELEASE_BRANCH"
    note "DRY RUN — would: wait for CI green, then tag + push + watch + confirm"
    restore_metadata
    trap - EXIT INT TERM HUP
    say "DRY RUN — reverted local edits; nothing pushed. Tree is clean."
    exit 0
fi

say "Committing release metadata + pushing $RELEASE_BRANCH"
git add VERSION include/axl/axl-version.h CHANGELOG.md
# THE MARKER IS WHY CI DOES NOT RUN ON THIS COMMIT.
#
# ci.yml triggers on every push to main, and cut-release.sh pushes this commit
# before tagging. Its content is a version string and a date: the gate below
# certifies it from its PARENT's runs (the release-only rule), and release.yml
# builds and tests it again on the tag, so running even the cheap push jobs
# here buys nothing.
#
# NOT `[skip ci]`. GitHub honours that natively, but it inspects the pushed
# HEAD commit -- and the TAG push carries this same commit, so it would very
# likely skip release.yml and docs.yml too and silently publish nothing. This
# marker is ours, GitHub gives it no meaning, and only ci.yml looks for it.
git commit -q -m "release: $TAG

[release-cut] ci.yml skips this commit: the release gate covers it from its
parent, and release.yml builds and tests it again on the tag. See RELEASING.md."
# The bump is committed — there is nothing left to restore, and a later failure
# (e.g. the push) must NOT roll the working tree back over the commit.
trap - EXIT INT TERM HUP
git push origin "$RELEASE_BRANCH"
REL_SHA="$(git rev-parse HEAD)"
note "pushed release commit $REL_SHA"

# ── the gate, ACTED ON ────────────────────────────────────────────────────
#
# Decided before the bump (above); the release commit changes only the version
# files, so what covered its parent covers it. What is left is the dispatch,
# when that is what the decision -- or --force-ci -- asked for.
case "$RELEASE_GATE_DECISION" in
    covered-local|covered-ci)
        say "Gate satisfied ($RELEASE_GATE_DECISION) — no CI dispatch"
        ;;
    untested)
        cat >&2 <<EOF

WARNING: tagging $TAG with NOTHING certifying it (--tag-untested). The release
notes say so. To certify the tree after the fact, dispatch the suite:
    gh workflow run ci.yml --ref $RELEASE_BRANCH

EOF
        ;;
    dispatch)
        # A push runs no suite since §14, so trigger it on the release commit
        # and wait for it before tagging.
        say "Triggering CI on $RELEASE_BRANCH (--ci-gate)"
        gh workflow run ci.yml --ref "$RELEASE_BRANCH" \
            || die "could not dispatch ci.yml (gh auth / workflow name?)"
        sleep 10   # let the dispatched run register before polling
        if ! wait_for_ci "$REL_SHA"; then
            cat >&2 <<EOF

CI did NOT pass on the release commit — NOT tagging (a published tag can't be
re-cut; see docs/RELEASING.md "Recovery"). The 'release: $TAG' commit is on
origin/$RELEASE_BRANCH. To recover:
  1. fix the failure on $RELEASE_BRANCH (normal commits) and push;
  2. run:  scripts/cut-release.sh $VERSION --resume --ci-gate$RESUME_HINT
     (it dispatches the suite on the new tip and waits for it)
EOF
            exit 1
        fi
        ;;
    *)  die "internal: no release-gate decision reached the tag step" ;;
esac

tag_and_publish "$REL_SHA"
