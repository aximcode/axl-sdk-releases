#!/bin/bash
# check-release-semver.sh — refuse to date a "### Breaking" section into a
# release whose version number does not admit one.
#
# Usage: scripts/check-release-semver.sh X.Y.Z [--allow-breaking]
#        (run from the repo root; reads ./VERSION and ./CHANGELOG.md)
#
# Exit 0 if the cut is consistent, 1 if it is not.
#
# WHY THIS EXISTS. cut-release.sh had exactly one CHANGELOG precondition --
# that a "## Unreleased" heading EXISTS -- and then `sed`-renamed that heading
# to "## X.Y.Z — DATE" over whatever sat beneath it. But "## Unreleased" is
# BRANCH-WIDE state and a release is a COMMIT RANGE; the two agree only when
# the release is "everything on main since the last tag". This project stopped
# working that way three releases ago, cutting 3.2.1, 3.2.2 and 3.2.3 from tags
# because main carried work that did not belong in a patch.
#
# So v3.2.3 was first cut from main and dated 43 commits of in-progress
# toolchain work into a PATCH release, two of them under "### Breaking". It
# published eight assets before anyone read the version number against the
# content. (Recovered: zero downloads, deleted, re-cut from the v3.2.2 tag.)
#
# WHY A REFUSAL AND NOT A WARNING. The information was never missing.
# cut-release.sh already prints the entire commit range before its confirm
# prompt; 43 commits scrolled past and the prompt was answered anyway -- and
# `--yes` skips the prompt outright, so on that path no human sees anything.
# The generated tag message even NAMED the breaking entries. Showing more does
# not work; the burden has to invert. That is this tree's own doctrine, from
# check-log-levels.py: make the exceptional case say so out loud.
#
# WHAT IT DOES NOT POLICE. Only "### Breaking" against a non-major bump. A
# stricter rule -- "### Added" forbidden on a patch, per semver -- was
# considered and rejected: it would fire on ordinary, correct releases (3.2.2
# added a build gate and a style-guide section as a patch, harmlessly), and a
# check that cries wolf gets `--allow-breaking` pasted in reflexively, which is
# the same failure as demanding a marker on all 129 legitimate warnings. A
# refusal is only worth having if it is rare enough to be read. An "### Added"
# on a patch is reported as a NOTE, and nothing more.
#
# The label is the input, and the labels were already right: both offending
# v3.2.3 entries were correctly filed under "### Breaking" by their authors.
# Nobody read them. This reads them.

set -euo pipefail

VERSION="${1:-}"
ALLOW_BREAKING=false
for arg in "${@:2}"; do
    case "$arg" in
        --allow-breaking) ALLOW_BREAKING=true ;;
        *) echo "ERROR: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: $0 X.Y.Z [--allow-breaking]" >&2
    exit 2
fi
[[ -f VERSION && -f CHANGELOG.md ]] \
    || { echo "ERROR: run from the repo root (need ./VERSION and ./CHANGELOG.md)" >&2; exit 2; }

PREV="$(cat VERSION)"

# major | minor | patch — how $VERSION differs from $PREV.
bump_level() {
    local a b c d
    IFS=. read -r a b _ <<<"$1"
    IFS=. read -r c d _ <<<"$2"
    if [[ "$a" != "$c" ]]; then echo major
    elif [[ "$b" != "$d" ]]; then echo minor
    else echo patch
    fi
}

# ONLY the section about to be dated. Scoping matters: a whole-file grep would
# hit every "### Breaking" this project has ever released and refuse every
# future patch, which is how a check earns its way into being deleted.
SECTION="$(awk '/^## Unreleased$/ {f=1; next} /^## / {f=0} f' CHANGELOG.md)"

LEVEL="$(bump_level "$PREV" "$VERSION")"

if grep -q '^### Breaking' <<<"$SECTION" && [[ "$LEVEL" != major ]]; then
    if $ALLOW_BREAKING; then
        echo "check-release-semver: '### Breaking' present and $PREV -> $VERSION is a" \
             "$LEVEL bump — allowed by --allow-breaking"
    else
        cat >&2 <<EOF
ERROR: CHANGELOG '## Unreleased' has a '### Breaking' section, but
       $PREV -> $VERSION is a $LEVEL bump.

       Breaking entries under it:
$(grep -A3 '^### Breaking' <<<"$SECTION" | grep '^- ' | sed 's/^/         /' | cut -c1-78)

       "## Unreleased" is branch-wide state; a release is a commit range.
       They agree only when the release is everything on this branch since
       the last tag. If it is not -- because main carries work that does not
       belong in this release -- cut from the previous TAG on a release
       branch instead, which is how 3.2.1, 3.2.2 and 3.2.3 shipped.

       Either cut a major, move those entries, or re-cut from a tag.
       Deliberate exception: --allow-breaking
EOF
        exit 1
    fi
fi

if grep -q '^### Added' <<<"$SECTION" && [[ "$LEVEL" == patch ]]; then
    echo "check-release-semver: NOTE — '### Added' in a patch release." \
         "Not refused, but semver would call new functionality a minor."
fi

echo "check-release-semver: OK — $PREV -> $VERSION ($LEVEL) is consistent with the section"
