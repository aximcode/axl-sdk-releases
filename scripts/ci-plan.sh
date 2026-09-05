#!/bin/bash
# ci-plan.sh — classify a push, so CI can run what the change can actually break.
#
# WHY THIS IS A SCRIPT AND NOT A WORKFLOW EXPRESSION. A `paths-ignore:` or an
# `if:` in YAML is logic that no local gate can see -- `which-gates.sh` says so
# for every workflow edit -- and this particular logic decides whether the
# suite runs at all. Getting it wrong is silent in the direction that matters.
# Here it is shell, with test-ci-plan.sh driving every class through a fixture
# repo, and ci.yml calls it.
#
# WHAT IT DECIDES, from `git diff --name-only base..head`:
#
#   docs_only=true     every changed path is a SAFE doc input -> the docs job
#                      alone is enough. Nothing else can be affected.
#   docs_touched=true  some changed path feeds the doc build -> the docs job
#                      should run, whatever else does.
#
# THE SAFE SET IS AN ALLOWLIST, and it is deliberately small, because the
# failure it guards is invisible: a path wrongly called "just docs" skips the
# suite. AXL-CI-Release-Speed-Design.md §11.6 paid for this list once already,
# rejecting the obvious `'**/*.md'`:
#
#   - the ROOT README.md is NOT safe. Three things read it: Sphinx includes it
#     (docs/sphinx/index.rst), check-tool-docs.py asserts every shipped tool
#     has a row in its table, and test-toolchain-variant.sh asserts it
#     documents no `make CROSS=` build. Two of those are tests, not doc builds.
#   - docs/sphinx/** is NOT safe: check-doc-coverage.py reads all 104 .rst
#     files to decide whether every public header is wired in, and that runs in
#     the lint job.
#   - src/*/README.md IS safe. Grepped for readers before allowlisting it, per
#     §11.6's own warning that "prose is not a synonym for nothing depends on
#     it": the only consumer is Sphinx's `.. include::` in the module pages.
#   - docs/**.md IS safe -- with one exception handled below.
#
# A DELETION IS NEVER SAFE. test-source-snapshot.sh asserts the published
# snapshot CONTAINS docs/AXL-Design.md; editing it cannot break that, removing
# it can. Rather than enumerate which doc files something asserts the existence
# of, any removed path falls back to the full run.
#
# FAIL-SAFE DIRECTION: anything unrecognised, an empty diff, or a diff that
# cannot be computed all yield docs_only=false. The cost of being wrong that
# way is a run nobody needed; the other way is a tag nothing tested.
#
# --list-unsafe <base> <head> prints, one per line, every changed path that is
# NOT in the safe set -- plus every REMOVED path, safe or not. The release gate
# uses it to ask a question this script deliberately does not answer on its
# own: "is everything here either prose or the version bump?" Keeping the
# allowlist here and the release-file list there means neither grows a copy of
# the other, which is the drift this tree keeps paying for.
#
# Usage: scripts/ci-plan.sh [--list-unsafe] <base> <head>
set -uo pipefail

SELF_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE=classify
if [[ "${1:-}" == "--list-unsafe" ]]; then MODE=list-unsafe; shift; fi
BASE="${1:-}"
HEAD_REF="${2:-}"
if [[ -z "$BASE" || -z "$HEAD_REF" ]]; then
    echo "usage: ci-plan.sh <base> <head>" >&2
    exit 2
fi

emit() {
    printf 'docs_only=%s\ndocs_touched=%s\nci_only=%s\nonly_tests=%s\n' \
           "$1" "$2" "${3:-false}" "${4:-}"
}

# WHICH TESTS READ THE WORKFLOW FILES -- derived, never written down. A
# hand-kept list here would go stale the first time a new test starts reading
# a workflow, which is the defect this whole script exists to prevent one level
# up. Over-inclusion is the safe direction: a test that only MENTIONS .github
# in a comment gets run, which costs seconds.
ci_reader_tests() {
    local d="$SELF_DIR/../test/integration"
    [[ -d "$d" ]] || return 0
    grep -rl -- '\.github/workflows' "$d"/test-*.sh 2>/dev/null \
        | xargs -r -n1 basename 2>/dev/null | sort -u | paste -sd, -
}

# A diff we could not compute is not an empty diff. Both are the same empty
# string and opposite facts, so the status is what decides.
if ! changed=$(git diff --name-only "$BASE" "$HEAD_REF" 2>/dev/null); then
    # A diff we could not compute must not read as "nothing unsafe changed"
    # either, so the listing mode fails rather than printing an empty set.
    [[ "$MODE" == "list-unsafe" ]] && exit 1
    emit false false
    exit 0
fi
[[ -n "$changed" ]] || { [[ "$MODE" == "list-unsafe" ]] && exit 0; emit false false; exit 0; }

# Deletions and renames: `git diff --diff-filter=D` lists what went away.
removed=$(git diff --name-only --diff-filter=D "$BASE" "$HEAD_REF" 2>/dev/null)

# is_safe_doc <path> — in the allowlist above.
is_safe_doc() {
    case "$1" in
        README.md)          return 1 ;;   # the root one; three readers
        docs/sphinx/*)      return 1 ;;   # check-doc-coverage.py reads these
        # ONE pattern, not three: `*` in a case glob crosses `/`, so this
        # already matches docs/notes/handoff.md at any depth. The
        # docs/sphinx/* arm above it is what keeps .rst files out, and
        # ordering is what makes that work.
        docs/*.md)          return 0 ;;
        src/*/README.md)    return 0 ;;
        *)                  return 1 ;;
    esac
}

# is_doc_input <path> — feeds the doc build, safe or not.
is_doc_input() {
    case "$1" in
        README.md)          return 0 ;;   # Sphinx includes it at index.rst:20
        docs/sphinx/*)      return 0 ;;
        # ONE pattern, not three: `*` in a case glob crosses `/`, so this
        # already matches docs/notes/handoff.md at any depth. The
        # docs/sphinx/* arm above it is what keeps .rst files out, and
        # ordering is what makes that work.
        docs/*.md)          return 0 ;;
        src/*/README.md)    return 0 ;;
        *)                  return 1 ;;
    esac
}

if [[ "$MODE" == "list-unsafe" ]]; then
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        is_safe_doc "$f" || printf '%s\n' "$f"
    done <<< "$changed"
    # A removal is never safe -- see the header. Printed even when the path is
    # in the allowlist, so a caller cannot wave it through.
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        is_safe_doc "$f" && printf '%s\n' "$f"
    done <<< "$removed"
    exit 0
fi

only=true
touched=false
# CI-ONLY IS A DIFFERENT KIND OF CLAIM from "probably just prose", and that is
# why it is allowed where "shell changed" is not. §12.5's warning is about
# GUESSING which tests a change can reach; this is not a guess: NOTHING BUILDS
# FROM .github. There is no path from a .yml file to a produced artifact, so
# the only things such a change can reach are CI itself and the tests that READ
# those files -- and that set is derived above rather than assumed.
ci=true
while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    is_doc_input "$f" && touched=true
    is_safe_doc  "$f" || only=false
    [[ "$f" == .github/* ]] || ci=false
done <<< "$changed"

# Any removal drops back to the full run -- see the header.
[[ -n "$removed" ]] && only=false

# A ci-only push runs the readers and nothing else -- unless the derivation
# came back EMPTY, which means the grep found nothing and is indistinguishable
# from "no test reads a workflow". Running everything is the only safe reading
# of that, so it falls back rather than skipping the suite on a blank list.
_readers=""
if [[ "$ci" == "true" ]]; then
    _readers="$(ci_reader_tests)"
    [[ -n "$_readers" ]] || ci=false
fi

emit "$only" "$touched" "$ci" "$_readers"
