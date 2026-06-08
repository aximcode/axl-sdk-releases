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
RELEASES_REPO="aximcode/axl-sdk-releases"

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --yes|-y)  ASSUME_YES=true ;;
        --resume)  RESUME=true ;;
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
    echo "usage: $0 X.Y.Z [--dry-run] [--yes] [--resume]" >&2
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
gh auth status >/dev/null 2>&1 || die "gh not authenticated (gh auth login)"

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
    say "Waiting for CI to pass on $sha (the release gate)"
    for i in $(seq 1 90); do          # 90 * 20s = 30 min ceiling
        line="$(gh run list --commit "$sha" --workflow CI \
                  --json status,conclusion \
                  --jq '.[0] | "\(.status):\(.conclusion)"' 2>/dev/null || true)"
        case "$line" in
            completed:success) note "CI: SUCCESS"; return 0 ;;
            completed:*)       note "CI: ${line#completed:}"; return 1 ;;
            *)                 note "[poll $i] CI: ${line:-no run yet}" ;;
        esac
        sleep 20
    done
    note "timed out waiting for CI"
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

    say "Watching CI + Release + Docs for $TAG"
    if ! scripts/watch-release-runs.sh "$TAG"; then
        die "a release workflow did not succeed — see the output above and 'gh run list'"
    fi

    say "Published release"
    gh release view "$TAG" --repo "$RELEASES_REPO" \
        --json name,tagName,isDraft,assets \
        --jq '.name, .tagName, ("draft=" + (.isDraft|tostring)), ("assets=" + (.assets|length|tostring))'
    say "$TAG is live on $RELEASES_REPO. Done."
}

# --------------------------------------------------------------------------
# --resume: main already carries the release commit + green CI — just tag.
# --------------------------------------------------------------------------
if $RESUME; then
    [[ "$(cat VERSION)" == "$VERSION" ]] || die "VERSION is $(cat VERSION), not $VERSION — nothing to resume"
    [[ "$(git rev-parse HEAD)" == "$(git rev-parse origin/main 2>/dev/null)" ]] \
        || die "HEAD != origin/main — push main first"
    wait_for_ci "$(git rev-parse HEAD)" || die "CI is not green on origin/main HEAD"
    tag_and_publish "$(git rev-parse HEAD)"
    exit 0
fi

# --------------------------------------------------------------------------
# normal flow
# --------------------------------------------------------------------------
grep -q '^## Unreleased' CHANGELOG.md \
    || die "CHANGELOG.md has no '## Unreleased' section — add release notes first"

PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || true)"
say "Will release $TAG — commits since ${PREV_TAG:-the beginning}"
git --no-pager log --oneline "${PREV_TAG:+$PREV_TAG..}HEAD" | sed 's/^/  /'

if ! $ASSUME_YES && ! $DRY_RUN; then
    printf '\nCut and publish %s? This pushes main and creates a public tag. [y/N] ' "$TAG"
    read -r reply
    [[ "$reply" == "y" || "$reply" == "Y" ]] || die "aborted by user"
fi

say "Bumping version + dating CHANGELOG"
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
    note "DRY RUN — would: git commit -m 'release: $TAG' && git push origin main"
    note "DRY RUN — would: wait for CI green, then tag + push + watch + confirm"
    git checkout -- VERSION include/axl/axl-version.h CHANGELOG.md
    say "DRY RUN — reverted local edits; nothing pushed. Tree is clean."
    exit 0
fi

say "Committing release metadata + pushing main"
git add VERSION include/axl/axl-version.h CHANGELOG.md
git commit -q -m "release: $TAG"
git push origin main
REL_SHA="$(git rev-parse HEAD)"
note "pushed release commit $REL_SHA"

if ! wait_for_ci "$REL_SHA"; then
    cat >&2 <<EOF

CI did NOT pass on the release commit — NOT tagging (a published tag can't be
re-cut; see docs/RELEASING.md "Recovery"). The 'release: $TAG' commit is on
origin/main. To recover:
  1. fix the failure on main (normal commits) and push;
  2. wait for CI to go green;
  3. run:  scripts/cut-release.sh $VERSION --resume
EOF
    exit 1
fi

tag_and_publish "$REL_SHA"
