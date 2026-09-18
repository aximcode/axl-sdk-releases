#!/bin/bash
# cut-release.sh — one-command release cut, per docs/RELEASING.md.
#
# Automates the whole flow so a release is "run one command, wait one CI
# cycle, done" — and ENFORCES the quality gate: it will not create the tag
# until CI is green on the release commit (the docs/RELEASING.md §4b rule that
# a public tag can't be cleanly re-cut once Release publishes).
#
# Usage:
#   scripts/cut-release.sh X.Y.Z            # cut a release
#   scripts/cut-release.sh X.Y.Z --dry-run  # show what it would do, change nothing
#   scripts/cut-release.sh X.Y.Z --yes      # skip the confirmation prompt
#   scripts/cut-release.sh X.Y.Z --allow-breaking  # cut a documented breaking
#                                           #   change under a non-major version
#   scripts/cut-release.sh X.Y.Z --watch    # wait for Release+Docs instead of
#                                           #   being handed a URL
#   scripts/cut-release.sh X.Y.Z --resume   # recovery: the release branch
#                                           #   already has the release commit +
#                                           #   green CI — just tag, push, watch
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
#                       section
#   2. bump-version   : VERSION + axl-version.h (via scripts/bump-version.sh)
#   3. date CHANGELOG : "## Unreleased" -> "## X.Y.Z — YYYY-MM-DD"
#   4. check-version  : make check-version
#   5. commit + push  : one "release: vX.Y.Z" commit -> origin/<release branch>
#   6. GATE           : wait for CI to go GREEN on the release commit
#   7. tag + push     : annotated vX.Y.Z (body summarised from the CHANGELOG)
#   8. watch + confirm: scripts/watch-release-runs.sh, then gh release view
#
# It does NOT run the heavy local prerequisite gate — CI on the release
# commit (step 6) is the authoritative gate. If main has un-CI'd work you want
# to fail-fast on, run the docs/RELEASING.md prerequisites by hand first.

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
# The stamp helper is shared with run-integration.sh so the two cannot
# drift on the format. Sourced, not re-implemented.
# shellcheck source=../test/integration/lib/release-gate.sh
source "$(dirname "$0")/../test/integration/lib/release-gate.sh"

CI_GATE=false   # ci.yml runs on a push to main and via workflow_dispatch (NOT
                # on release tags), so the release gate is the LOCAL suite plus
                # a CI run watched green before tagging (docs/RELEASING.md §4b).
                # --ci-gate makes this script dispatch ci.yml on the release
                # branch and wait for it. A --branch cut turns it on itself:
                # a push to a non-main branch starts no run at all.
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

if [[ -z "$VERSION" ]]; then
    echo "usage: $0 X.Y.Z [--branch release-X.Y.Z|release/X.Y.x] [--dry-run] [--yes]" \
         "[--resume] [--ci-gate] [--force-ci] [--watch] [--allow-breaking] [--no-docs-check]" >&2
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
    for i in $(seq 1 150); do
        line="$(gh run list --commit "$sha" --workflow CI \
                  --json status,conclusion \
                  --jq '.[0] | "\(.status):\(.conclusion)"' 2>/dev/null || true)"
        case "$line" in
            completed:success)
                # RUN-LEVEL SUCCESS IS NOT ENOUGH. GitHub reports an all-skipped
                # run as success, and the `[release-cut]` marker makes exactly
                # that shape reachable — "tag code no suite ever ran". The
                # stamp helper already asserts every job succeeded AND that the
                # full-suite job was among them; use it rather than a weaker
                # second opinion. On a release branch this is the only gate.
                if _release_gate_ci_green "$sha"; then note "CI: SUCCESS"; return 0; fi
                # TWO CAUSES, AND THIS CANNOT TELL THEM APART: the helper is
                # quiet, returning 1 both when the jobs say no full suite ran
                # and when their listing could not be read at all. Name both,
                # and hand over the run so a human can settle it in one click.
                note "CI reported success, but its jobs do not show a full-suite run"
                note "(or could not be read). Refusing — check the run:"
                note "    $(gh run list --commit "$sha" --workflow CI --limit 1 \
                              --json url --jq '.[0].url' 2>/dev/null \
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
# --resume: the release branch already carries the release commit + green CI
# (or, on a non-main branch, a CI run this dispatches) — just tag.
# --------------------------------------------------------------------------
if $RESUME; then
    [[ "$(cat VERSION)" == "$VERSION" ]] || die "VERSION is $(cat VERSION), not $VERSION — nothing to resume"
    [[ "$(git rev-parse HEAD)" == "$(git rev-parse "origin/$RELEASE_BRANCH" 2>/dev/null)" ]] \
        || die "HEAD != origin/$RELEASE_BRANCH — push $RELEASE_BRANCH first"
    if $CI_GATE; then
        RESUME_SHA="$(git rev-parse HEAD)"
        # DISPATCH BELONGS TO THE BRANCH, NOT TO THE CODE PATH. A push to a
        # non-main branch starts no ci.yml run at all, so a resume that only
        # WAITED would poll for 75 minutes for a run that cannot appear — and
        # the recovery text below sends you here. Skipped when a green run
        # already covers this sha, which is the post-timeout resume the 75-min
        # ceiling exists for: that case must still return in seconds.
        if [[ "$RELEASE_BRANCH" != "main" ]] && _release_gate_ci_green "$RESUME_SHA"; then
            # ANSWERED, so do not ask a second, weaker question. wait_for_ci
            # reads the NEWEST run at the sha whatever its conclusion, while
            # this helper accepts any completed+success run whose jobs all ran:
            # with a failed re-run sitting on top of a green one they disagree,
            # and asking both would print "already covered" and then die "not
            # green" about the same commit.
            note "a green CI run already covers $RESUME_SHA — no dispatch needed"
        else
            if [[ "$RELEASE_BRANCH" != "main" ]]; then
                say "Dispatching CI on $RELEASE_BRANCH (a push to it starts no run)"
                gh workflow run ci.yml --ref "$RELEASE_BRANCH" \
                    || die "could not dispatch ci.yml (gh auth / workflow name?)"
                sleep 10   # let the dispatched run register before polling
            fi
            wait_for_ci "$RESUME_SHA" || die "CI is not green on origin/$RELEASE_BRANCH HEAD"
        fi
    fi
    tag_and_publish "$(git rev-parse HEAD)"
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
restore_metadata() {
    git checkout -- VERSION include/axl/axl-version.h CHANGELOG.md 2>/dev/null || true
}
trap restore_metadata EXIT INT TERM HUP

scripts/bump-version.sh "$VERSION" >/dev/null
TODAY="$(date +%Y-%m-%d)"
# Date only the FIRST "## Unreleased" (the active section).
sed -i.bak "0,/^## Unreleased$/s//## $VERSION — $TODAY/" CHANGELOG.md
rm -f CHANGELOG.md.bak
note "VERSION -> $VERSION ; CHANGELOG '## Unreleased' -> '## $VERSION — $TODAY'"

make check-version >/dev/null && note "check-version OK"

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
# before tagging -- so a release ran the whole integration suite twice on the
# same box: once as the LOCAL pre-release gate (which is the authoritative one,
# see RELEASING.md) and again from this push, minutes later, for a commit whose
# entire content is a version string and a date. The tag then builds and tests
# everything a third time in release.yml.
#
# NOT `[skip ci]`. GitHub honours that natively, but it inspects the pushed
# HEAD commit -- and the TAG push carries this same commit, so it would very
# likely skip release.yml and docs.yml too and silently publish nothing. This
# marker is ours, GitHub gives it no meaning, and only ci.yml looks for it.
git commit -q -m "release: $TAG

[release-cut] ci.yml skips this commit: the local suite gated it before the
cut, and release.yml builds and tests it again on the tag. See RELEASING.md."
# The bump is committed — there is nothing left to restore, and a later failure
# (e.g. the push) must NOT roll the working tree back over the commit.
trap - EXIT INT TERM HUP
git push origin "$RELEASE_BRANCH"
REL_SHA="$(git rev-parse HEAD)"
note "pushed release commit $REL_SHA"

# ── the gate, consulted ALWAYS ────────────────────────────────────────────
#
# TWO SOURCES OF GREEN, and the second is the one that removes the ~8.5 minutes
# a human used to wait. `ci.yml` runs the identical uncached suite on every push
# to main, on the same self-hosted box, and goes green on the commit this
# release is cut from -- and the stamp reader could not see any of it, because
# it reads a local file only an uncached LOCAL run writes. So the only way to
# satisfy the gate was to run that suite a SECOND time here, and --ci-gate
# would then dispatch a THIRD (AXL-CI-Release-Speed-Design.md §4.4/§10.3).
#
# CONSULTED EVEN WITHOUT --ci-gate. The check is free and the answer is the one
# fact that decides whether this tag is verified; only the DISPATCH is gated by
# the flag. Reporting nothing when nothing covers the commit is how a release
# comes to be gated by a human's memory.
GATE_WHY=""
GATE_SRC=""
if ! $FORCE_CI; then
    if GATE_WHY="$(release_gate_covers "$REL_SHA")"; then
        GATE_SRC="local suite"
    elif GATE_WHY="$(release_gate_ci_covers "$REL_SHA")"; then
        GATE_SRC="CI"
    else
        GATE_SRC=""
    fi
fi
if [[ -n "$GATE_SRC" ]]; then
    say "Gate satisfied by the $GATE_SRC — $GATE_WHY"
elif ! $CI_GATE; then
    # NOT a `note`. Nothing certified this tree, and the next line of output is
    # a public tag that cannot be re-cut.
    cat >&2 <<EOF

WARNING: nothing has certified $REL_SHA.
         no local stamp, and no green CI run at it or its parent.
         Tagging anyway, because --ci-gate was not passed.
         To certify it, either:
           ./test/integration/run-integration.sh --no-cache
           git push          # and let CI go green on $RELEASE_BRANCH
         or re-run this with --ci-gate to dispatch and wait.

EOF
fi

if $CI_GATE && ! $FORCE_CI && [[ -n "$GATE_SRC" ]]; then
    # ALREADY GREEN — skip the dispatch entirely. The local suite is the
    # authoritative gate (RELEASING.md), and it stamped this tree; dispatching
    # CI would run the same tests on the same code for an answer we hold.
    # That is the one move that improves turnaround AND cost at once, since a
    # run never dispatched costs nothing and takes no time
    # (AXL-CI-Release-Speed-Design.md §10.3/§10.4).
    #
    # It reads the stamp against the RELEASE commit, not HEAD-before-the-bump:
    # release_gate_covers accepts a descendant whose whole diff is VERSION,
    # axl-version.h and CHANGELOG.md, which is exactly what the bump touches.
    note "skipping the CI dispatch (pass --force-ci to run it anyway)"
elif $CI_GATE; then
    # No usable stamp. Since 2026-08-19 the commonest reason is not "you did
    # not run the suite" but "you ran it WITH THE CACHE" -- caching is on by
    # default (run-integration.sh), and a cached run deliberately refuses to
    # stamp, because it skipped tests whose inputs merely looked unchanged.
    # Say so before dispatching, or the fix reads as "run it again" and the
    # next run is cached too.
    say "Nothing covers $REL_SHA -- no local stamp, and no green CI run at it"
    say "or its parent."
    note "a CACHED run cannot stamp -- for the local gate use:"
    note "    ./test/integration/run-integration.sh --no-cache --arch X64"
    note "    ./test/integration/run-integration.sh --no-cache --arch AARCH64"
    note "dispatching CI instead (this is correct, just slower)"
    # CI no longer auto-runs on a push, so trigger it on the release commit and
    # wait for it before tagging.
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
  2. wait for CI to go green;
  3. run:  scripts/cut-release.sh $VERSION --resume --ci-gate$RESUME_HINT
EOF
        exit 1
    fi
else
    note "CI gate SKIPPED — the LOCAL suite is the release gate now."
    note "Run './test/integration/run-integration.sh -j\$(nproc)' before cutting"
    note "(see docs/RELEASING.md). Pass --ci-gate to wait on a manual CI run."
fi

tag_and_publish "$REL_SHA"
