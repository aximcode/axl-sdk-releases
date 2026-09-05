#!/bin/bash
# axl-desc: print which gates a change actually needs (advisory; runs nothing)
#
# which-gates.sh — read a diff, print the gate to run. PRINTS, NEVER RUNS.
#
# WHY THIS EXISTS. `CLAUDE.md`'s Build gates section says "prefer verify.sh over
# hand-running these individually", which is right about not typing 27 gate
# names and was read as "always run everything". Stacked with the integration
# suite and a separate docs build, a prose fix cost what a library change costs:
# the suite is 201 tests and its ten dearest declare ~1,260 s between them.
#
# The judgement "which gate can SEE this change?" is correct but is made dozens
# of times a session, often by someone with no memory of the last time. This
# turns it into a command.
#
# IT PRINTS AND EXITS. A gate that decides what to check is a gate that can
# decide wrong, and this tree has paid repeatedly for checks that could not see
# (`feedback_a_gate_that_cannot_see`). The human keeps the veto; the output is
# a suggestion with its reasoning attached, not an instruction.
#
# THE COVERING-TEST LOOKUP IS DERIVED, NOT A TABLE. A hardcoded map of
# file -> test is a second source of truth that drifts the moment a test is
# renamed. Instead we grep `test/` for the changed file's basename, which is
# how the covering test actually refers to it. A table would have been wrong
# about `test-install-lifecycle.sh` twice during the host-toolchain work.
#
# OUTPUT TEXT IS AN INTERFACE. A changed message string can be exact-matched by
# a test in a completely unrelated file: `test-install-lifecycle.sh:546` did
# `[ "$tcstate" = "MISSING" ]` against a label another commit relabelled to
# `g++:MISSING`, and the fix wave had scoped that suite out on the honest but
# wrong grounds that it had touched no podman code. So for shell and C changes
# this also greps for *quoted strings the diff removed*, which is the cheap
# approximation of "what consumes what I changed".
#
# Usage:
#   scripts/which-gates.sh              # working tree vs HEAD (staged + unstaged)
#   scripts/which-gates.sh <base>       # everything since <base>, e.g. origin/main
#   scripts/which-gates.sh <base> <head>
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

case $# in
    0) FILES=$(git diff --name-only HEAD; git diff --cached --name-only) ;;
    1) FILES=$(git diff --name-only "$1") ;;
    *) FILES=$(git diff --name-only "$1" "$2") ;;
esac
FILES=$(printf '%s\n' "$FILES" | sort -u | grep -v '^$' || true)

if [[ -z "$FILES" ]]; then
    echo "which-gates: no changes — nothing to gate."
    exit 0
fi

# Classify. A file may land in more than one bucket; that is deliberate, since
# the union of what a change touches is what has to be checked.
want_docs=0 want_lint_make=0 want_full_verify=0 note_ci=0 note_release=0
covering=""      # integration tests that name a changed file
vague=""         # files whose name is too common to attribute coverage from
changed_shell=0 changed_c=0

while read -r f; do
    [[ -n "$f" ]] || continue
    case "$f" in
        docs/*.md|*.md|docs/sphinx/*)          want_docs=1 ;;&
        src/*|include/*|test/unit/*|tools/*|sdk/examples/*)
                                               want_full_verify=1; changed_c=1 ;;&
        Makefile|scripts/*.mk)                 want_full_verify=1 ;;&
        scripts/*.py)                          want_lint_make=1 ;;&
        scripts/axl|scripts/axl-cc|scripts/axl-c++|scripts/install.sh|\
        scripts/install-toolchain.sh|scripts/axl-prune.sh|packaging/install.sh|\
        scripts/*.sh)                          changed_shell=1 ;;&
        .github/workflows/*)                   note_ci=1 ;;&
        VERSION|CHANGELOG.md)                  note_release=1 ;;&
    esac
    # DERIVED covering tests: who names this file?
    #
    # PATH FIRST, basename only as a fallback, and a CEILING on both. A bare
    # basename over-matches catastrophically: `basename scripts/axl` is "axl",
    # which appears in 190 of 201 test files, and a list of 190 "covering
    # tests" is not coverage — it is the loose-grep failure this tree already
    # records (feedback_loose_greps_hid_three_shipped_bugs) dressed as help.
    # Above the ceiling we say we CANNOT ATTRIBUTE rather than print noise
    # shaped like an answer.
    b=$(basename "$f")
    hits=$(grep -rlF -- "$f" test/ 2>/dev/null | grep '\.sh$' || true)
    if [[ -z "$hits" && ${#b} -ge 8 ]]; then
        hits=$(grep -rlwF -- "$b" test/ 2>/dev/null | grep '\.sh$' || true)
    fi
    n=$(printf '%s' "$hits" | grep -c . || true)
    if [[ "$n" -gt 12 ]]; then
        vague="$vague$f ($n)"$'\n'
    elif [[ -n "$hits" ]]; then
        covering="$covering$hits"$'\n'
    fi
done <<< "$FILES"

covering=$(printf '%s' "$covering" | sort -u | grep -v '^$' || true)

echo "which-gates — $(printf '%s\n' "$FILES" | wc -l) file(s) changed"
echo
printf '%s\n' "$FILES" | sed 's/^/    /'
echo
echo "RUN:"

if [[ $want_full_verify -eq 1 ]]; then
    echo "    ./scripts/verify.sh            # C/C++/headers/Makefile — all three jobs"
elif [[ $want_lint_make -eq 1 && $want_docs -eq 1 ]]; then
    echo "    ./scripts/verify.sh            # gates AND prose both reachable"
elif [[ $want_lint_make -eq 1 ]]; then
    echo "    ./scripts/verify.sh --only=lint,make"
elif [[ $want_docs -eq 1 ]]; then
    echo "    ./scripts/verify.sh --only=docs   # ~1m54s vs ~10m for the full run"
fi

if [[ -n "$covering" ]]; then
    echo "    # covering tests (they NAME a file you changed) — run these directly,"
    echo "    # not the whole suite: run-integration.sh has no --only=<test>."
    printf '%s\n' "$covering" | sed 's|^|    ./|'
fi

if [[ -n "$vague" ]]; then
    echo "    # CANNOT ATTRIBUTE a covering test for these — the name is too common"
    echo "    # to grep for, and a list that broad would not be coverage. Find the"
    echo "    # real consumer by grepping the SYMBOL or STRING you changed:"
    printf '%s' "$vague" | sed 's/^/    #   /'
fi

if [[ $want_full_verify -eq 0 && $want_docs -eq 0 && $want_lint_make -eq 0 && -z "$covering" && -z "$vague" ]]; then
    echo "    (nothing matched — say so rather than running everything by reflex,"
    echo "     and if that is wrong, the classifier above is what to fix)"
fi

echo
echo "DO NOT:"
echo "    ./scripts/build-docs.sh       # runs INSIDE verify.sh (verify.sh:121) — Sphinx twice"
if [[ $want_full_verify -eq 0 ]]; then
    echo "    ./test/integration/run-integration.sh   # full suite: 201 tests. Pre-push, not per-edit."
fi

# The trap that cost this tree a real regression.
if [[ $changed_shell -eq 1 || $changed_c -eq 1 ]]; then
    removed=$( { case $# in
        0) git diff HEAD; git diff --cached ;;
        1) git diff "$1" ;;
        *) git diff "$1" "$2" ;;
      esac; } 2>/dev/null | grep '^-' | grep -v '^---' \
      | grep -oE '"[A-Za-z][A-Za-z0-9 :._/+-]{6,}"' | sort -u | head -8 || true)
    if [[ -n "$removed" ]]; then
        echo
        echo "CHECK — your diff REMOVES these strings. Output text is an interface:"
        echo "a test in an unrelated file may exact-match one (that is how"
        echo "test-install-lifecycle.sh:546 broke). grep before you scope a suite out:"
        printf '%s\n' "$removed" | sed 's/^/    /'
        echo "    grep -rn <string> test/ scripts/ .github/"
    fi
fi

echo
echo "Pre-push: ./test/integration/run-integration.sh   (cache ON, DEFAULT workers)"
[[ $note_release -eq 1 ]] && echo "Release cut: add --no-cache. See docs/RELEASING.md."
[[ $note_ci -eq 1 ]] && echo "NOTE: you changed a workflow — it only runs on push/tag, so a local gate cannot see it."
exit 0
