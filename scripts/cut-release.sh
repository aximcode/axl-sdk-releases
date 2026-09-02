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
#   scripts/cut-release.sh X.Y.Z --resume   # recovery: main already has the
#                                           #   release commit + green CI — just
#                                           #   tag, push, watch, confirm
#
# What it does (normal flow):
#   1. preconditions  : on main, clean tree, gh authed, tag absent, CHANGELOG
#                       has an "## Unreleased" section
#   2. bump-version   : VERSION + axl-version.h (via scripts/bump-version.sh)
#   3. date CHANGELOG : "## Unreleased" -> "## X.Y.Z — YYYY-MM-DD"
#   4. check-version  : make check-version
#   5. commit + push  : one "release: vX.Y.Z" commit -> origin/main
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

CI_GATE=false   # CI runs only via workflow_dispatch (NOT on push, NOT on
                # release tags), so the release gate is the LOCAL suite plus a
                # manual CI dispatch on main watched green before tagging (see
                # docs/RELEASING.md §4b). --ci-gate makes this script dispatch
                # ci.yml on main and wait for it green before it tags.
RELEASES_REPO="aximcode/axl-sdk-releases"
# Deliberate exception to the semver refusal below: a release that really is
# meant to carry a documented breaking change under a non-major version.
ALLOW_BREAKING=""
# The pre-tag docs gate is ON by default; see where it runs for why.
RUN_DOCS_CHECK=true

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --yes|-y)  ASSUME_YES=true ;;
        --resume)  RESUME=true ;;
        --ci-gate) CI_GATE=true ;;
        --force-ci) CI_GATE=true; FORCE_CI=true ;;
        --allow-breaking) ALLOW_BREAKING=1 ;;
        --no-docs-check)  RUN_DOCS_CHECK=false ;;
        -*)        echo "ERROR: unknown flag '$arg'" >&2; exit 2 ;;
        *)
            if [[ -n "$VERSION" ]]; then
                echo "ERROR: version given twice ('$VERSION', '$arg')" >&2; exit 2
            fi
            VERSION="$arg"
            ;;
    esac
done

if [[ -z "$VERSION" ]]; then
    echo "usage: $0 X.Y.Z [--dry-run] [--yes] [--resume] [--allow-breaking] [--no-docs-check]" >&2
    exit 2
fi
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: version must be X.Y.Z (got '$VERSION')" >&2
    exit 2
fi

TAG="v$VERSION"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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

[[ "$(git branch --show-current)" == "main" ]] || die "not on branch main"

# Working tree must be clean (gitignored files like CLAUDE.md / .claude are fine).
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    git status --short
    die "working tree has uncommitted tracked changes"
fi

git tag -l "$TAG" | grep -q . && die "tag $TAG already exists locally"
if git ls-remote --tags origin "$TAG" 2>/dev/null | grep -q .; then
    die "tag $TAG already exists on origin"
fi
note "on main, clean tree, $TAG is free, gh authenticated"

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
            completed:success) note "CI: SUCCESS"; return 0 ;;
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
        note "DRY RUN — would: scripts/watch-release-runs.sh $TAG"
        note "DRY RUN — would: gh release view $TAG --repo $RELEASES_REPO"
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
    # CI is not triggered by the TAG -- but it is triggered by the `git push
    # origin main` this script does two steps earlier, and that push carries the
    # release commit. So a CI run on this same SHA is expected and the watcher
    # will see it; it just is not something the tag started.
    local expect="Release Docs"
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
# --resume: main already carries the release commit + green CI — just tag.
# --------------------------------------------------------------------------
if $RESUME; then
    [[ "$(cat VERSION)" == "$VERSION" ]] || die "VERSION is $(cat VERSION), not $VERSION — nothing to resume"
    [[ "$(git rev-parse HEAD)" == "$(git rev-parse origin/main 2>/dev/null)" ]] \
        || die "HEAD != origin/main — push main first"
    $CI_GATE && { wait_for_ci "$(git rev-parse HEAD)" || die "CI is not green on origin/main HEAD"; }
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

PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || true)"
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
    printf '\nCut and publish %s? This pushes main and creates a public tag. [y/N] ' "$TAG"
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
    note "DRY RUN — would: git commit -m 'release: $TAG' (+ [release-cut]) && git push origin main"
    note "DRY RUN — would: wait for CI green, then tag + push + watch + confirm"
    restore_metadata
    trap - EXIT INT TERM HUP
    say "DRY RUN — reverted local edits; nothing pushed. Tree is clean."
    exit 0
fi

say "Committing release metadata + pushing main"
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
git push origin main
REL_SHA="$(git rev-parse HEAD)"
note "pushed release commit $REL_SHA"

if $CI_GATE && ! $FORCE_CI && release_gate_covers "$REL_SHA" >/dev/null 2>&1; then
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
    say "CI gate satisfied locally — $(release_gate_covers "$REL_SHA")"
    note "skipping the CI dispatch (pass --force-ci to run it anyway)"
elif $CI_GATE; then
    # No usable stamp. Since 2026-08-19 the commonest reason is not "you did
    # not run the suite" but "you ran it WITH THE CACHE" -- caching is on by
    # default (run-integration.sh), and a cached run deliberately refuses to
    # stamp, because it skipped tests whose inputs merely looked unchanged.
    # Say so before dispatching, or the fix reads as "run it again" and the
    # next run is cached too.
    say "No uncached local stamp covers $REL_SHA."
    note "a CACHED run cannot stamp -- for the local gate use:"
    note "    ./test/integration/run-integration.sh --no-cache --arch X64"
    note "    ./test/integration/run-integration.sh --no-cache --arch AARCH64"
    note "dispatching CI instead (this is correct, just slower)"
    # CI no longer auto-runs on a push, so trigger it on the release commit and
    # wait for it before tagging.
    say "Triggering CI on main (--ci-gate)"
    gh workflow run ci.yml --ref main || die "could not dispatch ci.yml (gh auth / workflow name?)"
    sleep 10   # let the dispatched run register before polling
    if ! wait_for_ci "$REL_SHA"; then
        cat >&2 <<EOF

CI did NOT pass on the release commit — NOT tagging (a published tag can't be
re-cut; see docs/RELEASING.md "Recovery"). The 'release: $TAG' commit is on
origin/main. To recover:
  1. fix the failure on main (normal commits) and push;
  2. wait for CI to go green;
  3. run:  scripts/cut-release.sh $VERSION --resume --ci-gate
EOF
        exit 1
    fi
else
    note "CI gate SKIPPED — the LOCAL suite is the release gate now."
    note "Run './test/integration/run-integration.sh -j\$(nproc)' before cutting"
    note "(see docs/RELEASING.md). Pass --ci-gate to wait on a manual CI run."
fi

tag_and_publish "$REL_SHA"
