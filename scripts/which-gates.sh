#!/bin/bash
# axl-desc: print which gates a change actually needs (advisory; runs nothing)
#
# which-gates.sh — read a diff, print the gate to run. PRINTS, NEVER RUNS.
#
# WHY THIS EXISTS. `CLAUDE.md`'s Build gates section says "prefer verify.sh over
# hand-running these individually", which is right about not typing 27 gate
# names and was read as "always run everything". Stacked with the integration
# suite and a separate docs build, a prose fix cost what a library change costs:
# the suite is 204 tests and its ten dearest declare ~1,260 s between them.
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
#   scripts/which-gates.sh --files a.c b.sh     # ask about these paths, no diff
#   scripts/which-gates.sh --porcelain ...      # key=value, for another program
#
# --porcelain is the machine-readable rendering of the SAME decision the prose
# makes -- not a second classifier. It exists because
# `run-integration.sh --for=<paths>` needs this lookup and the alternative was
# regexing the prose, which makes every wording change a breaking change.
# The keys:
#
#   jobs=x64,aa64,...   the verify.sh --only= list for these files ("" = none)
#   cover=<path>        a file under test/ that NAMES one of these files
#   vague=<path> (<n>)  too common a name to attribute coverage from
#   missing=<path>      a path that is not in the worktree at all
#
# `cover=` also names a size-gated test for a path under its declared
# `size-gate-paths=` prefixes, though the test names no such path -- see
# SIZE GATES below.
#
# `cover=` IS NOT FILTERED to what a runner can run, deliberately. It reports
# what covers the file -- which legitimately includes lib/*.sh helpers and
# *-selftest.sh files, both of which a human should know about and neither of
# which lib/discover.sh selects. Deciding what is RUNNABLE belongs to the
# runner, which owns discovery; doing it here would be a second, drifting copy
# of that judgement.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

PORCELAIN=0
FILES_SEEN=0
declare -a POS=() EXPLICIT=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --porcelain) PORCELAIN=1; shift ;;
        # FILES_SEEN apart from the list. `--files` with nothing after it,
        # or `--files $VAR` with VAR unset, used to leave EXPLICIT empty and
        # fall through to the git-diff branch below -- silently answering a
        # DIFFERENT question (the working tree) than the one asked. That is
        # the defect recorded at the removed-strings check further down,
        # reintroduced one flag over.
        --files) shift; FILES_SEEN=1
                 while [[ $# -gt 0 && "$1" != --* ]]; do EXPLICIT+=("$1"); shift; done ;;
        -h|--help) awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next }
                        NR>1 { exit }' "$0"; exit 0 ;;
        -*) echo "which-gates: unknown option $1" >&2; exit 2 ;;
        *)  POS+=("$1"); shift ;;
    esac
done

if [[ $FILES_SEEN -eq 1 && ${#EXPLICIT[@]} -eq 0 ]]; then
    echo "which-gates: --files was given no paths. Reading the git diff" \
         "instead would answer a different question than the one asked." >&2
    exit 2
fi

# A path that is not in the worktree is NOT the same fact as a path nothing
# covers, and a caller that cannot tell them apart will report a typo as
# "no test covers this". Collected before classification so the answer names
# every bad path, not just the first.
MISSING=""
if [[ ${#EXPLICIT[@]} -gt 0 ]]; then
    for f in "${EXPLICIT[@]}"; do
        [[ -e "$f" ]] || MISSING="$MISSING$f"$'\n'
    done
    FILES=$(printf '%s\n' "${EXPLICIT[@]}")
else
    # RESOLVE EVERY REF FIRST, and refuse one this repository does not have.
    # `git diff --name-only HEAD~1` in a SHALLOW clone prints a fatal to
    # stderr and NOTHING to stdout, and this script then reported "no
    # changes — nothing to gate" and exited 0: a gate advisor answering
    # "nothing to check" because it could not look. That is the exact shape
    # this file exists to refuse, and it reached CI -- actions/checkout
    # defaults to fetch-depth 1, so HEAD~1 does not exist there, which is
    # how the gate-scoping self-test failed on CI while passing locally.
    for _r in "${POS[@]}"; do
        if ! git rev-parse --verify --quiet "$_r^{commit}" >/dev/null; then
            echo "which-gates: cannot resolve '$_r' in this repository." >&2
            echo "  A shallow clone (actions/checkout defaults to" \
                 "fetch-depth: 1) has no history to diff against." >&2
            echo "  Name paths instead: which-gates.sh --files <paths>" >&2
            exit 2
        fi
    done
    case ${#POS[@]} in
        0) FILES=$(git diff --name-only HEAD; git diff --cached --name-only) ;;
        1) FILES=$(git diff --name-only "${POS[0]}") ;;
        *) FILES=$(git diff --name-only "${POS[0]}" "${POS[1]}") ;;
    esac
fi
# ONE SPELLING per path, before anything compares them: `./src/x.c` and an
# absolute path inside this repo are the same file as `src/x.c`, and every
# match below (the path grep, the size gates) is a literal string compare.
_norm=""
while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    f=${f#"$PWD"/}                       # exact: $PWD is not a pattern here
    while [[ "$f" == ./* ]]; do f=${f#./}; done
    _norm="$_norm$f"$'\n'
done <<< "$FILES"
FILES=$(printf '%s' "$_norm" | sort -u | grep -v '^$' || true)
MISSING=$(printf '%s' "$MISSING" | sort -u | grep -v '^$' || true)

if [[ -n "$MISSING" && $PORCELAIN -eq 1 ]]; then
    while IFS= read -r m; do [[ -n "$m" ]] && echo "missing=$m"; done <<<"$MISSING"
fi

# The HUMAN gets the warning too. It was porcelain-only, which is backwards:
# prose is the mode CLAUDE.md points people at, and a typo'd path there was
# silently classified and could still recommend jobs.
if [[ -n "$MISSING" && $PORCELAIN -eq 0 ]]; then
    echo "which-gates: WARNING — not in the worktree (typo?):"
    printf '%s\n' "$MISSING" | sed 's/^/    /'
    echo
fi

if [[ -z "$FILES" ]]; then
    if [[ $PORCELAIN -eq 1 ]]; then
        echo "jobs="
        exit 0
    fi
    echo "which-gates: no changes — nothing to gate."
    exit 0
fi

# Classify. A file may land in more than one bucket; that is deliberate, since
# the union of what a change touches is what has to be checked.
want_docs=0 want_lint_make=0 want_gates=0 want_full_verify=0 note_ci=0 note_release=0
# The docs job reads EXACTLY two things (docs/sphinx/Doxyfile: INPUT is
# ../../include/axl, FILE_PATTERNS *.h *.hpp, RECURSIVE NO; plus the
# `.. include:: ../../../src/*/README.md` lines in docs/sphinx/modules/*.rst).
# So a .c file, an internal src/ header, include/uefi/*, tools/, test/unit/ and
# sdk/examples/ are INVISIBLE to it, and running Sphinx for them buys nothing.
# That is the difference between want_code and want_docs below: the older
# version ran all six jobs for anything under src/ or include/.
want_code=0
# The driver idle-budget timing cases void under verify.sh's concurrent load,
# so only verify.sh's quiet slot (job qloop) can judge them. A change to what
# they test -- the loop, the idle-notify backend, its counter and GUID, the
# clock -- or to the test and its skip/check helpers, routes there.
want_qloop=0
covering=""      # integration tests that name a changed file
vague=""         # files whose name is too common to attribute coverage from
changed_shell=0 changed_c=0 changed_md=0

# The text of every _axl_gate(...) call in the gate registration (calls span
# lines; collected up to the paren that closes each). A registration that
# cannot be read is said, not routed as "no gate runs this".
GATE_CALLS=""
if [[ -r cmake/AxlIntegrationTests.cmake.in ]]; then
    GATE_CALLS=$(awk '
        /_axl_gate\(/ && !/macro\(_axl_gate/ { on = 1; depth = 0 }
        on { print; n = gsub(/\(/, "("); m = gsub(/\)/, ")"); depth += n - m
             if (depth <= 0) on = 0 }' cmake/AxlIntegrationTests.cmake.in)
fi
[[ -n "$GATE_CALLS" ]] || echo "which-gates: WARNING: no _axl_gate call read from" \
    "cmake/AxlIntegrationTests.cmake.in -- a gate's own files route to nothing" >&2

while read -r f; do
    [[ -n "$f" ]] || continue
    case "$f" in
        docs/sphinx/*)                         want_docs=1 ;;&
        include/axl/*.h|include/axl/*.hpp)     want_docs=1 ;;&   # Doxygen's only INPUT
        *.md)                                  changed_md=1 ;;&
        src/*|include/*|test/unit/*|tools/*|sdk/examples/*)
                                               want_code=1; want_full_verify=1; changed_c=1 ;;&
        scripts/*.py)                          want_lint_make=1 ;;&
        scripts/axl|scripts/axl-cc|scripts/axl-c++|scripts/install.sh|\
        scripts/install-toolchain.sh|scripts/axl-prune.sh|packaging/install.sh|\
        scripts/*.sh)                          changed_shell=1 ;;&
        # A GATE'S OWN IMPLEMENTATION maps to the gate it implements.
        # Without this, editing scripts/build-docs.sh -- the script that IS
        # the zero-warning docs job -- matched only the `scripts/*.sh` arm
        # below, which sets no job at all, so this printed "(nothing
        # matched)" for a change to a gate. Now that CLAUDE.md tells a reader
        # to run what this script names, that silence was an instruction to
        # skip it.
        scripts/build-docs.sh)                 want_docs=1 ;;&
        src/loop/*|src/backend/native/axl-backend-native-event.c|\
        src/backend/native/axl-backend-native.c|src/backend/axl-backend.h|\
        include/uefi/axl-uefi-extra.h|src/util/axl-time.c|\
        test/unit/axl-test-loop.c|test/unit/axl-test.h)
                                               want_qloop=1 ;;&
        scripts/lint.sh|scripts/check-clang-warnings.py|.clang-tidy)
                                               want_lint_make=1 ;;&
        # The CMake build: EVERY directory's CMakeLists.txt, not just the
        # root's -- the literal `CMakeLists.txt` arm routed a directory's own
        # file to no job at all. Lint is the x64-lint preset.
        # Not the consumer's shipped example project, nor vendored code's own.
        sdk/examples/CMakeLists.txt|deps/*/*)  ;;&
        CMakeLists.txt|*/CMakeLists.txt|CMakePresets.json|cmake/*)
            case "$f" in
                sdk/examples/*|deps/*/*) ;;
                *) want_lint_make=1 ;;
            esac ;;&
        .github/workflows/*)                   note_ci=1 ;;&
        VERSION|CHANGELOG.md)                  note_release=1 ;;&
        # THE GATES' OWN IMPLEMENTATION: the registration, and the helper the
        # compile-only gates share. The CMake build files route here too: a
        # build edit can add a link input to sshd (check-size-gate-paths, an
        # artifact gate) or move what an artifact gate reads.
        cmake/AxlIntegrationTests.cmake.in|scripts/compile-like.py|\
        CMakeLists.txt|*/CMakeLists.txt|CMakePresets.json|cmake/*)
            case "$f" in
                sdk/examples/*|deps/*/*) ;;
                *) want_gates=1 ;;
            esac ;;&
    esac
    # ...and any file a GATE runs, read off the registration rather than
    # listed: a gate's scripts/check-<x>.sh (moved out of the Makefile) is
    # that gate's implementation, and was routed nowhere (review,
    # 2026-11-08). Only the text of `_axl_gate(...)` calls counts -- the same
    # file also registers gate.docs, gate.lint and gate.qloop, whose scripts
    # have jobs of their own.
    if [[ -n "$GATE_CALLS" ]] && grep -qF "/$f\"" <<< "$GATE_CALLS"; then
        want_gates=1
    fi
    # Is this markdown file actually an input to the docs job? DERIVED, and
    # DO NOT replace this with a list. Sphinx pulls in exactly what its .rst
    # files `.. include::`, which today is the root README.md, 33
    # src/*/README.md AND TWELVE docs/*.md (AXL-Design.md, AXL-SDK-Design.md,
    # AXL-Coding-Style.md, ROADMAP.md, RELEASING.md and the rest, via
    # docs/sphinx/guides/*.rst) -- but the whole point is that the grep
    # answers, not the enumeration. The first version of this comment said
    # "the root README.md and src/*/README.md and NOTHING else under docs/",
    # which was wrong about twelve files and pointed a reader away from a
    # ZERO-WARNING gate; the code was right the whole time. What IS true is
    # that a file no .rst includes -- a handoff, a vendored
    # deps/*/README.md -- is read by no gate, and `--only=docs` for one is
    # ~57s of Sphinx that cannot see it.
    if [[ "$f" == *.md ]] && grep -rqF -- "$f" docs/sphinx/ 2>/dev/null; then
        want_docs=1
    fi

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
    # PROSE is the one case where a mention in a COMMENT is not attribution.
    # A test documents which source file it covers in its comments -- that is
    # how most of this lookup works and why it is not filtered in general --
    # but no test "covers" a design doc that way. It can only consume prose by
    # READING it, which is a command, not a comment. Without this, editing
    # CLAUDE.md proposed test-input-keys-qemu.sh, which merely cites it in a
    # comment. `prose can be a test input` still holds: a test that greps a
    # doc does so on a non-comment line and is still reported.
    if [[ "$f" == *.md && -n "$hits" ]]; then
        kept=""
        while read -r t; do
            [[ -n "$t" ]] || continue
            # `grep … | grep -qv …` would be the natural spelling and is a
            # TRAP here: `set -o pipefail` is on (see the top of this file),
            # `-q` exits the moment it decides, the upstream grep takes
            # SIGPIPE, and the pipeline reports 141 -- so a file that DOES
            # have a non-comment mention can read as if it had none, which
            # would silently drop a doc's real coverage. `grep -c` reads to
            # the end and cannot be short-circuited.
            if (( $(grep -F -- "$f" "$t" 2>/dev/null | grep -cv '^[[:space:]]*#') > 0 )); then
                kept="$kept$t"$'\n'
            elif (( $(grep -wF -- "$b" "$t" 2>/dev/null | grep -cv '^[[:space:]]*#') > 0 )); then
                kept="$kept$t"$'\n'
            fi
        done <<< "$hits"
        hits=$(printf '%s' "$kept")
    fi
    n=$(printf '%s' "$hits" | grep -c . || true)
    if [[ "$n" -gt 12 ]]; then
        vague="$vague$f ($n)"$'\n'
    elif [[ -n "$hits" ]]; then
        covering="$covering$hits"$'\n'
    fi
done <<< "$FILES"

# SIZE GATES: a test that caps an image's size covers every file LINKED into
# that image, and no test names those files -- nothing in
# test-efi-pack-release-qemu.sh mentions src/loop/axl-loop.c, yet a loop
# change grew sshd.efi past its limit and the grep above could not see it
# (twice). The test DECLARES what can change that image as path prefixes
# (`size-gate-paths=src/,deps/,tools/sshd,...` in its header, less the
# `size-gate-exclude=*.md` globs prose under them needs), and this is a
# literal prefix match: no make, no cache, nothing from the environment, so
# it cannot go blind or serve a stale answer. What keeps the declaration
# honest is gate.check-size-gate-paths (scripts/size-gate-inputs.sh --tree
# build/x64-debug --check), which derives the linked set from the CMake
# tree's own record (ninja's inputs and deps) and fails when a file falls
# outside it.
sized=""         # "<test> <- <file>" lines, for the prose explanation
# shellcheck source=../test/integration/lib/discover.sh
source test/integration/lib/discover.sh
while IFS= read -r sg; do
    [[ -n "$sg" && -n "$(test_meta_field "$sg" size-gate)" ]] || continue
    hit=0
    while IFS= read -r f; do
        if size_gate_routes "$sg" "$f"; then
            sized="$sized$sg <- $f"$'\n'; hit=1
        fi
    done <<< "$FILES"
    [[ $hit -eq 1 ]] && covering="$covering$sg"$'\n'
done < <(grep -l '^# test-meta:.*[[:space:]]size-gate=' test/integration/test-*.sh 2>/dev/null)

covering=$(printf '%s' "$covering" | sort -u | grep -v '^$' || true)

# ONE decision, two renderings. The job list is computed here and both the
# prose below and --porcelain print THIS variable -- a porcelain branch that
# re-derived the list would be a second classifier able to disagree with the
# advice a human reads.
ALL_JOBS=x64,aa64,lint,gates,imggates,qloop,docs
JOBS_CSV=""
if [[ $want_code -eq 1 && $want_docs -eq 1 ]]; then
    JOBS_CSV="$ALL_JOBS"
elif [[ $want_code -eq 1 ]]; then
    JOBS_CSV=x64,aa64,lint,gates,imggates
    [[ $want_qloop -eq 1 ]] && JOBS_CSV="$JOBS_CSV,qloop"
elif [[ $want_full_verify -eq 1 ]]; then
    JOBS_CSV="$ALL_JOBS"
elif [[ $want_lint_make -eq 1 && $want_docs -eq 1 ]]; then
    JOBS_CSV="$ALL_JOBS"
elif [[ $want_gates -eq 1 && $want_docs -eq 1 ]]; then
    JOBS_CSV="$ALL_JOBS"
elif [[ $want_lint_make -eq 1 && $want_gates -eq 1 ]]; then
    JOBS_CSV=lint,gates,imggates
elif [[ $want_lint_make -eq 1 ]]; then
    JOBS_CSV=lint,gates
elif [[ $want_gates -eq 1 ]]; then
    JOBS_CSV=gates,imggates
elif [[ $want_docs -eq 1 ]]; then
    JOBS_CSV=docs
fi

if [[ $PORCELAIN -eq 1 ]]; then
    echo "jobs=$JOBS_CSV"
    [[ -n "$covering" ]] && while IFS= read -r c; do
        [[ -n "$c" ]] && echo "cover=$c"
    done <<<"$covering"
    [[ -n "$vague" ]] && while IFS= read -r v; do
        [[ -n "$v" ]] && echo "vague=$v"
    done <<<"$vague"
    exit 0
fi

echo "which-gates — $(printf '%s\n' "$FILES" | wc -l) file(s) changed"
echo
printf '%s\n' "$FILES" | sed 's/^/    /'
echo
echo "RUN:"

if [[ $want_code -eq 1 && $want_docs -eq 1 ]]; then
    echo "    ./scripts/verify.sh            # code AND a doc input (a public header"
    echo "                                   # or a src/*/README.md) — every job"
elif [[ $want_code -eq 1 ]]; then
    echo "    ./scripts/verify.sh --only=$JOBS_CSV"
    echo "                                   # every job but docs: Doxygen reads only"
    echo "                                   # include/axl/*.h and src/*/README.md,"
    echo "                                   # so Sphinx cannot see this change"
elif [[ $want_full_verify -eq 1 ]]; then
    echo "    ./scripts/verify.sh            # every job"
elif [[ $want_lint_make -eq 1 && $want_docs -eq 1 ]]; then
    echo "    ./scripts/verify.sh            # gates AND prose both reachable"
elif [[ $want_lint_make -eq 1 || $want_gates -eq 1 ]]; then
    echo "    ./scripts/verify.sh --only=$JOBS_CSV"
elif [[ $want_docs -eq 1 ]]; then
    echo "    ./scripts/verify.sh --only=$JOBS_CSV   # ~57s vs 111s for the full run"
fi

# The prose list is what a human pastes into `run-integration.sh --only=`, which
# REFUSES the whole list for one name discovery did not find -- and `covering`
# legitimately holds helpers (release-fixture.sh) and *-selftest.sh files. So
# split it with discovery's own name predicate (lib/discover.sh), shared with
# the runner, never a hand-kept exclusion. Only the NAME is asked: a test the
# arch=/local-only= gates drop on one leg is still a test some leg runs, which
# is why this does not need the (slow) per-header pass. Nothing is discovered
# at all when nothing is covered.
runnable=""; notrunnable=""
if [[ -n "$covering" ]]; then
    while IFS= read -r c; do
        [[ -n "$c" ]] || continue
        if discover_is_test_name "$c" && [[ -f "$c" ]]; then
            runnable="$runnable$c"$'\n'
        else
            notrunnable="$notrunnable$c"$'\n'
        fi
    done <<<"$covering"
fi

if [[ -n "$runnable" ]]; then
    echo "    # covering tests (they NAME a file you changed, or size-gate an image"
    echo "    # that links it -- see SIZE GATE below) — run these directly,"
    echo "    # not the whole suite. Either run them directly, or name them:"
    echo "    #   ./test/integration/run-integration.sh --only=a.sh,b.sh"
    echo "    # ...or have the runner ask this script for the same list:"
    echo "    #   ./test/integration/run-integration.sh --for=<the paths you changed>"
    printf '%s\n' "$runnable" | grep -v '^$' | sed 's|^|    ./|'
fi

if [[ -n "$sized" ]]; then
    echo "    # SIZE GATE -- these tests cap an image that can LINK a file you changed"
    echo "    # (the test's size-gate-paths=, held to the build by"
    echo "    # gate.check-size-gate-paths):"
    printf '%s' "$sized" | sort -u | sed 's|^|    #   |'
fi

if [[ -n "$notrunnable" ]]; then
    echo "    # also NAME a file you changed, but run-integration.sh cannot run them;"
    echo "    # do NOT put these in --only:"
    discover_drop_explanation | sed 's/^/    #   /'
    printf '%s\n' "$notrunnable" | grep -v '^$' | sed 's|^|    #   ./|'
fi

if [[ -n "$vague" ]]; then
    echo "    # CANNOT ATTRIBUTE a covering test for these — the name is too common"
    echo "    # to grep for, and a list that broad would not be coverage. Find the"
    echo "    # real consumer by grepping the SYMBOL or STRING you changed:"
    printf '%s' "$vague" | sed 's/^/    #   /'
fi

if [[ $want_full_verify -eq 0 && $want_docs -eq 0 && $want_lint_make -eq 0 && $want_gates -eq 0 && -z "$covering" && -z "$vague" ]]; then
    if [[ $changed_md -eq 1 ]]; then
        echo "    (nothing — no docs/sphinx/*.rst includes this file, so the docs"
        echo "     job cannot see it and --only=docs would run Sphinx over content"
        echo "     it does not read. NOTE what it DOES include: the root README.md,"
        echo "     src/*/README.md, and twelve docs/*.md such as AXL-Design.md and"
        echo "     ROADMAP.md — those DO get --only=docs, and this line is not about"
        echo "     them. Read yours yourself; that is the only review prose gets.)"
    else
        echo "    (nothing matched — say so rather than running everything by reflex,"
        echo "     and if that is wrong, the classifier above is what to fix)"
    fi
fi

echo
echo "DO NOT:"
echo "    ./scripts/build-docs.sh       # runs INSIDE verify.sh (the docs job) — Sphinx twice"
if [[ $want_full_verify -eq 0 ]]; then
    echo "    ./test/integration/run-integration.sh   # full suite: 204 tests. Pre-push, not per-edit."
fi

# The trap that cost this tree a real regression.
#
# READS ${#POS[@]}, NOT $#. The option loop above consumes the arguments, so
# `case $# in` saw 0 here whatever was passed and silently diffed the working
# tree instead of the range asked for -- a check answering a question nobody
# asked. In --files mode there is no diff at all: the caller named paths, not
# a range, so this section has nothing to read and says nothing.
if [[ ${#EXPLICIT[@]} -eq 0 && ( $changed_shell -eq 1 || $changed_c -eq 1 ) ]]; then
    removed=$( { case ${#POS[@]} in
        0) git diff HEAD; git diff --cached ;;
        1) git diff "${POS[0]}" ;;
        *) git diff "${POS[0]}" "${POS[1]}" ;;
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
