#!/bin/bash
# test-meta: arch=x64 needs= est=5 local-only=0
# test-release-gate.sh — the stamp that lets a release skip a redundant CI run.
#
# This gate decides whether a release is verified. A false POSITIVE tags code
# no suite ever ran, so every case below is about refusing:
#   - no stamp, malformed stamp, a stamp from a FAILED run
#   - a stamp for an unrelated commit
#   - a stamp whose tree has since changed in anything but the version files
# and exactly one about accepting: the release commit's parent, whose only
# diff is VERSION / axl-version.h / CHANGELOG.md (design §10.3).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [[ "$2" == "$3" ]]; then pass "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
export AXL_STAMP_FILE="$TMP/stamp"
# shellcheck source=lib/release-gate.sh
source "$SCRIPT_DIR/lib/release-gate.sh"

# A throwaway repo, so the assertions do not depend on this tree's history.
REPO="$TMP/repo"; mkdir -p "$REPO"; cd "$REPO"
git init -q .; git config user.email t@t; git config user.name t
mkdir -p include/axl
echo 4.1.0 > VERSION; echo hdr > include/axl/axl-version.h
echo log > CHANGELOG.md; echo src > src.c
git add -A >/dev/null; git commit -qm base
BASE=$(git rev-parse HEAD)

# ── a stub `gh` ───────────────────────────────────────────────────────────
#
# BOTH halves of the gate ask GitHub now: the CI half whether a run with the
# suite went green, and the stamp half whether the CHEAP push jobs did
# (AXL-CI-Release-Speed-Design.md §14). Driven through a stub so it needs no
# network and no real run, and so the REFUSING cases -- which are the ones that
# matter, since a false positive tags code no suite ever ran -- can be produced
# on demand.
#
# THE STUB ANSWERS WHAT `gh --jq` WOULD PRINT, not raw JSON, and the decisions
# are made in shell rather than inside a jq expression -- so every one of them
# is reachable from here. `gh` embeds its own jq, so this adds no dependency.
mkdir -p "$TMP/bin"
GH_RUNS="$TMP/gh-runs.tsv"
GH_JOBS="$TMP/gh-jobs.txt"
GH_FAIL="$TMP/gh-fail"
cat > "$TMP/bin/gh" <<STUB
#!/bin/bash
# Two queries are made: the run list for a commit (TSV), and one run's jobs
# (one per line). \$GH_JOBS.<run-id>, when present, answers for that run alone,
# which is how two runs at ONE commit -- a push run and a dispatched one --
# can disagree. \$GH_FAIL makes every call fail the way an expired token does.
[[ -e "$GH_FAIL" ]] && { echo "gh: HTTP 401: Bad credentials" >&2; exit 1; }
prev=""
for a in "\$@"; do
    if [[ "\$prev" == "view" ]]; then
        if [[ -f "$GH_JOBS.\$a" ]]; then cat "$GH_JOBS.\$a"; else cat "$GH_JOBS"; fi
        exit 0
    fi
    prev="\$a"
done
cat "$GH_RUNS"
STUB
chmod +x "$TMP/bin/gh"
export PATH="$TMP/bin:$PATH"

# gh_says <run-tsv-lines> <job-tsv-lines> -- newline-terminated, the way a
# real `gh --jq` writes them. A job line is `<name>\t<conclusion>`: the NAME is
# part of the answer, because "every job passed" and "the suite ran" became
# different claims the moment a push could produce a partial run.
gh_reset() { rm -f "$GH_JOBS".* "$GH_FAIL"; }
gh_says() { gh_reset; printf '%s\n' "$1" > "$GH_RUNS"; printf '%s\n' "$2" > "$GH_JOBS"; }
# ...and the same without the final newline, which is what a truncated or
# differently-buffered answer looks like. `read` returns non-zero on such a
# line while still assigning it, so a bare `while read` drops it -- and here
# that line IS the answer.
gh_says_unterminated() { gh_reset; printf '%s' "$1" > "$GH_RUNS"; printf '%s' "$2" > "$GH_JOBS"; }
run_row() { printf '%s\tcompleted\t%s\t%s' "$1" "$2" "${3:-4242}"; }   # <sha> <conclusion> [id]
# job_rows <cheap-conclusion> <suite-conclusion> -- a DISPATCHED run's shape:
# the three cheap jobs and the suite.
job_rows() {
    printf 'plan\tsuccess\ngcc x64\t%s\ngcc aa64\t%s\nclang-tidy\t%s\nQEMU integration (full suite)\t%s' \
        "$1" "$1" "$1" "$2"
}
# push_rows <cheap-conclusion> -- a PUSH run's shape since §14: the cheap jobs
# ran, and the suite job is listed SKIPPED (its `if:` wants workflow_dispatch).
push_rows() { job_rows "$1" skipped; }
# push_rows_without <job> -- a green push run with one cheap job missing.
push_rows_without() { push_rows success | grep -vF -- "$1"$'\t'; }

echo "=== release-gate ==="

release_gate_covers "$BASE" >/dev/null 2>&1
check "no stamp -> refuses" "$?" "1"

# The stamp half's accepting cases need a green PUSH run of the cheap jobs at
# the stamped commit -- see the section right below for why.
gh_says "$(run_row "$BASE" success)" "$(push_rows success)"
release_gate_write "$BASE" X64
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp on HEAD + green cheap push CI -> accepts" "$?" "0"

# ── a local stamp needs the CHEAP push CI too (§14) ──────────────────────
#
# Since 2026-10-21 a push runs only plan + gcc x64/aa64 + clang-tidy; the QEMU
# suite runs on workflow_dispatch. So a release can be gated by a full local
# uncached run INSTEAD of a dispatched suite -- but only alongside a green
# push run of the cheap jobs on the same tree, because those are what catch
# the 2026-08-18 class a dev box structurally cannot: a CLEAN CHECKOUT that
# does not link (§11.2). A dev box always has the stale objects that hide it.
GH_SAW_NOTHING="$(printf 'stamp matches HEAD (release-suite, X64, %s), but the cheap push CI does not cover it\nno green cheap push CI (gcc x64, gcc aa64, clang-tidy) at %s or an inert ancestor within 25 commits:\n  %s: no CI run' \
    "$(sed -n 's/^when=//p' "$AXL_STAMP_FILE")" "$BASE" "${BASE:0:8}")"
gh_says "" ""
out=$(release_gate_covers "$BASE" 2>&1); rc=$?
check "stamp on HEAD but NO push run -> refuses" "$rc" "1"
check "...and says the cheap push CI is what is missing" "$out" "$GH_SAW_NOTHING"

# Each cheap job is load-bearing on its own, so each one's absence refuses --
# and the full set (the control above) accepts, so these are about the JOB.
for _j in "gcc x64" "gcc aa64" "clang-tidy"; do
    gh_says "$(run_row "$BASE" success)" "$(push_rows_without "$_j")"
    release_gate_covers "$BASE" >/dev/null 2>&1
    check "stamp ok, push run lacks '$_j' -> refuses" "$?" "1"
done
gh_says "$(run_row "$BASE" success)" "$(push_rows failure)"
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp ok, cheap jobs FAILED -> refuses" "$?" "1"
gh_says "$(run_row "$BASE" success)" "$(push_rows skipped)"
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp ok, cheap jobs SKIPPED (docs-only / release-cut shape) -> refuses" "$?" "1"

# A push run STILL GOING is the commonest refusal right after a push, and the
# remedy is to wait, not to push again -- so it says so.
gh_says "$(printf '%s\tin_progress\t\t4242' "$BASE")" ""
out=$(release_gate_covers "$BASE" 2>&1); rc=$?
# Its own status (3), so a caller can WAIT rather than refuse outright.
check "stamp ok, push run still in progress -> still running (3)" "$rc" "3"
check "...and the header says the deciding run is still running" "$(sed -n 2p <<< "$out")" \
    "the cheap push CI (gcc x64, gcc aa64, clang-tidy) deciding $BASE is still running:"
check "...and says the run is still going" "$(sed -n 3p <<< "$out")" \
    "  ${BASE:0:8}: a CI run is still in_progress -- wait for it"

# "COULD NOT CHECK" IS NOT "NOT COVERED". Both refuse -- fail closed -- but a
# missing or broken gh says so, in its own exit status (2), rather than telling
# someone to push a commit that already has a green run.
gh_says "$(run_row "$BASE" success)" "$(push_rows success)"
# AXL_GH, not PATH=/nonexistent: the stamp half needs sed and git to get as
# far as asking, and a PATH with no tools refuses as "malformed stamp" instead
# -- the right exit status for the wrong reason.
out=$(AXL_GH="$TMP/no-such-gh" release_gate_covers "$BASE" 2>&1); rc=$?
check "stamp ok, no gh on PATH -> could not check (2)" "$rc" "2"
check "...and says it could not check, not that CI is red" "$(sed -n 2p <<< "$out")" \
    "could not check cheap push CI (gcc x64, gcc aa64, clang-tidy) for $BASE:"
touch "$GH_FAIL"
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp ok, gh FAILS (expired token) -> could not check (2)" "$?" "2"
gh_reset

# ONLY scripts/release-suite.sh CERTIFIES (review ruling R-A). A stamp in the
# old run-integration.sh format -- the suite alone, without the unit tests in
# QEMU, the staged SDK or the harness self-tests CI's job also runs -- is
# refused, and says why, rather than reading as "malformed".
gh_says "$(run_row "$BASE" success)" "$(push_rows success)"
printf 'sha=%s\narch=X64\npass=159\nfail=0\nwhen=2026-10-20T00:00:00Z\n' "$BASE" > "$AXL_STAMP_FILE"
out=$(release_gate_covers "$BASE" 2>&1); rc=$?
check "a run-integration.sh-format stamp -> refuses" "$rc" "1"
check "...and names release-suite.sh as what certifies" "$out" \
    "stamp was not written by scripts/release-suite.sh -- a bare run-integration.sh run does not certify a release"
# Control: the same sha through release_gate_write is accepted, so the refusal
# above is about the FORMAT and not the commit.
release_gate_write "$BASE" X64
release_gate_covers "$BASE" >/dev/null 2>&1
check "control: the same sha stamped by release_gate_write -> accepts" "$?" "0"

printf 'sha=\narch=X64\n' > "$AXL_STAMP_FILE"
release_gate_covers "$BASE" >/dev/null 2>&1
check "malformed stamp -> refuses" "$?" "1"

# The release commit: only the three version files change.
release_gate_write "$BASE" X64
gh_says "$(run_row "$BASE" success)" "$(push_rows success)"
echo 4.2.0 > VERSION; echo hdr2 > include/axl/axl-version.h; echo log2 > CHANGELOG.md
git add -A >/dev/null; git commit -qm "release: v4.2.0"
REL=$(git rev-parse HEAD)
release_gate_covers "$REL" >/dev/null 2>&1
check "release commit over a stamped parent + green cheap CI at it -> accepts" "$?" "0"

# The case that must never pass: real code changed after the stamp.
echo changed > src.c; git add -A >/dev/null; git commit -qm "a real change"
release_gate_covers "$(git rev-parse HEAD)" >/dev/null 2>&1
check "source changed since the stamp -> refuses" "$?" "1"

# A stamp for a commit that is not an ancestor at all.
git checkout -q -b other "$BASE"; echo x > other.c
git add -A >/dev/null; git commit -qm other
release_gate_write "$(git rev-parse HEAD)" X64
release_gate_covers "$REL" >/dev/null 2>&1
check "stamp from an unrelated commit -> refuses" "$?" "1"

# ── a CANCELLED code run under a docs tip (review ruling R-C) ─────────────
#
# A docs-only push cancels the in-progress push run of the code commit below
# it (cancel-in-progress). The tip's own run then skips the cheap jobs, and
# the walk back reaches a CANCELLED run: the gate must say so -- and that the
# fix is a re-dispatch or a re-push -- not refuse generically.
echo code2 > other.c; git add -A >/dev/null; git commit -qm "code under docs" >/dev/null
CXCODE=$(git rev-parse HEAD)
mkdir -p docs; echo prose > docs/cancel-note.md
git add -A >/dev/null; git commit -qm "docs on top" >/dev/null
CXDOCS=$(git rev-parse HEAD)
gh_says "$(printf '%s\n%s' "$(run_row "$CXDOCS" success 6002)" "$(run_row "$CXCODE" cancelled 6001)")" ""
push_rows skipped > "$GH_JOBS.6002"
push_rows cancelled > "$GH_JOBS.6001"
release_gate_write "$CXDOCS" X64
out=$(release_gate_covers "$CXDOCS" 2>&1); rc=$?
check "docs tip over a CANCELLED code run -> refuses" "$rc" "1"
check "...and names the cancellation and the remedy" "$(sed -n 4p <<< "$out")" \
    "  ${CXCODE:0:8}: run 6001 was cancelled -- re-dispatch or re-push"
# Control: the same shape with the code run green is accepted -- the walk
# reaches it -- so the refusal above is about the cancellation.
gh_says "$(printf '%s\n%s' "$(run_row "$CXDOCS" success 6002)" "$(run_row "$CXCODE" success 6001)")" ""
push_rows skipped > "$GH_JOBS.6002"
push_rows success > "$GH_JOBS.6001"
release_gate_covers "$CXDOCS" >/dev/null 2>&1
check "control: the same docs tip over a GREEN code run -> accepts" "$?" "0"
gh_reset

echo ""

# --- the release commit must skip ci.yml, and only ci.yml ------------------
#
# cut-release.sh pushes the release commit to main BEFORE tagging, and ci.yml
# triggers on push -- so a release ran the full integration suite twice on this
# box: once as the local pre-release gate, once from that push, for a commit
# whose content is a version string and a date. release.yml then builds and
# tests it a third time on the tag.
#
# The marker is spelled in two files that cannot import each other, which is
# the drift shape this tree keeps paying for. Hold them equal here.
MARK='[release-cut]'
CUT="$PROJECT_DIR/scripts/cut-release.sh"
CI="$PROJECT_DIR/.github/workflows/ci.yml"

grep -qF -- "$MARK" "$CUT"
check "cut-release.sh writes the skip marker" "$?" "0"

# THE GUARD NEEDS BOTH HALVES, and the reason is the one this file already
# names one layer over: a check "cannot survive its own subject being
# discussed". The guard used to be a bare substring test over the WHOLE commit
# message, body included -- so `d9dc7117`, a commit whose body EXPLAINED the
# marker, skipped every CI job. Silently: the run exists and reports `skipped`.
# Any commit documenting the mechanism disabled the thing it documents.
#
# The subject half fixes it, because a body is never the start of the message,
# and the marker half is kept so a commit merely TITLED `release: v...` by hand
# does not skip either. Both, or neither.
GUARD_SUBJ="startsWith(github.event.head_commit.message, 'release: v')"
GUARD_MARK="contains(github.event.head_commit.message, '$MARK')"
n_guard=$(grep -cF -- "$GUARD_SUBJ" "$CI")
n_mark=$(grep -cF -- "$GUARD_MARK" "$CI")
[[ "$n_guard" -eq "$n_mark" && "$n_guard" -gt 0 ]]
check "every guard tests the subject AND the marker ($n_guard / $n_mark)" "$?" "0"
! grep -qF -- "!contains(github.event.head_commit.message, '$MARK')" "$CI"
check "no job carries the bare body-substring guard any more" "$?" "0"
# Job keys are the 2-space-indented mappings under `jobs:`, and nothing else --
# an anchored `^  name:` regex over the whole file also catches step keys and
# matrix entries, which is how this first read "3 of 5".
n_jobs=$(awk '/^jobs:/{inj=1;next} /^[^[:space:]]/{inj=0}
              inj && /^  [A-Za-z][A-Za-z0-9_-]*:[[:space:]]*$/{n++}
              END{print n+0}' "$CI")
# Counted on the job `if:` lines alone: the run-level `cancel-in-progress`
# carries the same expression (a [release-cut] push must not cancel its
# parent's run), and it is not a job.
n_guard_if=$(grep -F -- "$GUARD_SUBJ" "$CI" | grep -c '^    if:')
_ok=1; [[ "$n_guard_if" -eq "$n_jobs" && "$n_guard_if" -gt 0 ]] && _ok=0
check "every ci.yml job is guarded by it ($n_guard_if of $n_jobs)" "$_ok" "0"

# ...and the commit cut-release.sh actually writes must SATISFY that guard.
# The two live in files that cannot import each other, so a subject convention
# in one and a predicate in the other is exactly the drift this file exists
# for -- and getting it wrong means the release commit re-runs the full suite
# for a version string, which is the cost the marker was added to remove.
grep -qF 'git commit -q -m "release: $TAG' "$CUT"
check "cut-release.sh writes a subject the guard matches (release: \$TAG)" "$?" "0"
grep -qE '^TAG="v\$VERSION"|TAG="v\$\{VERSION\}"' "$CUT"
check "...and TAG is v-prefixed, which is what startsWith looks for" "$?" "0"

# NOT `[skip ci]`: GitHub honours that natively by inspecting the pushed HEAD
# commit, and the TAG push carries the same commit -- it would skip release.yml
# and docs.yml too and publish nothing at all.
#
# Comments stripped first. The comment in cut-release.sh explaining why we do
# NOT use `[skip ci]` contains the string, so scanning the raw file finds it
# and fails -- a check that cannot survive its own subject being discussed.
! sed 's/#.*//' "$CUT" | grep -qiE '\[(skip ci|ci skip|no ci|skip actions|actions skip)\]'
check "the marker is OURS, not a GitHub-native skip string" "$?" "0"

# ...and the publish workflows must carry no such guard, or a release would
# silently not publish.
for _wf in release docs; do
    ! grep -qF -- "$MARK" "$PROJECT_DIR/.github/workflows/$_wf.yml"
    check "$_wf.yml does NOT skip on the marker" "$?" "0"
done

echo ""
echo "=== release-gate: reusing a green CI run ==="
# WHY THIS HALF EXISTS. A CI run with the suite in it -- since §14 that is a
# DISPATCHED run (`gh workflow run ci.yml`, by hand or by cut-release.sh) --
# is the same uncached suite on the same box, and the stamp half above cannot
# see it, because it reads a local file. Without this, a green dispatched run
# would still send the cut to a second local run, or `--ci-gate` to a THIRD.

gh_says "$(run_row "$BASE" success)" "$(job_rows success success)"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "a green CI run at the commit -> accepts" "$?" "0"

gh_says "$(run_row "$BASE" failure)" "$(job_rows success failure)"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "a FAILED CI run -> refuses" "$?" "1"

gh_says "" ""
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "no CI run at all -> refuses" "$?" "1"

# THE ONE THAT MATTERS MOST. The `release: vX.Y.Z` commit carries a
# [release-cut] marker and every ci.yml job is conditioned on NOT seeing it,
# so a CI run DOES exist at that commit and it tested nothing. Accepting it
# would tag code no suite ever ran -- the exact failure this whole file is
# about -- and it is reachable by the most ordinary path there is.
gh_says "$(run_row "$BASE" success)" "$(job_rows skipped skipped)"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "a run whose jobs all SKIPPED -> refuses" "$?" "1"

gh_says "$(run_row "$BASE" success)" "$(job_rows success skipped)"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "a run with ONE job skipped -> refuses" "$?" "1"

# A run with NO jobs at all is the same class of nothing, and it is what an
# empty answer from a query that failed looks like.
gh_says "$(run_row "$BASE" success)" ""
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "a run reporting no jobs -> refuses" "$?" "1"

# The parent rule, same as the stamp half: a green run at the parent carries
# to a release commit whose whole diff is the version files.
# CIPARENT IS CAPTURED, not assumed to be $BASE. The stamp half above leaves
# several commits on this branch, so a fixture that named $BASE as "the parent"
# was asserting against a commit two moves back -- it failed for a reason that
# had nothing to do with the rule under test.
CIPARENT=$(git rev-parse HEAD)
echo 4.2.0 > VERSION; echo hdr2 > include/axl/axl-version.h; echo log2 > CHANGELOG.md
git add -A >/dev/null; git commit -qm "release: v4.2.0"
CIREL=$(git rev-parse HEAD)
gh_says "$(run_row "$CIPARENT" success)" "$(job_rows success success)"
release_gate_ci_covers "$CIREL" >/dev/null 2>&1
check "a green run at the PARENT covers the release commit" "$?" "0"

# ...and stops covering the moment anything else moved. Same green run, one
# more commit, and that commit touches src.c.
echo more > src.c; git add -A >/dev/null; git commit -qm "src change"
CIREL2=$(git rev-parse HEAD)
gh_says "$(run_row "$CIPARENT" success)" "$(job_rows success success)"
release_gate_ci_covers "$CIREL2" >/dev/null 2>&1
check "a green run at an ancestor with other changes -> refuses" "$?" "1"

# THE SHAPE EVERY PUSH NOW HAS (§14), and the reason the gate learned job
# NAMES. A push runs plan + the cheap jobs, the suite job is listed SKIPPED,
# and the run concludes success. "The run passed" and "the suite ran" are
# different claims, and reading the first as the second tags code no suite
# ever touched -- by cutting a release after any ordinary push.
gh_says "$(run_row "$BASE" success)" "$(push_rows success)"
out=$(release_gate_ci_covers "$BASE" 2>&1); rc=$?
check "a green PUSH run (suite skipped) -> refuses" "$rc" "1"
# ...and it must say WHY and what to do, not just refuse: since §14 this is
# the commonest refusal there is, and the fix is one command.
check "...and names the missing job and the dispatch" "$out" \
"$(printf "no green full CI run (with 'QEMU integration (full suite)') at %s or an inert ancestor within 25 commits:\n  %s: run 4242: 'QEMU integration (full suite)' concluded skipped\nthe suite runs only on workflow_dispatch: gh workflow run ci.yml --ref main, then wait for it" "$BASE" "${BASE:0:8}")"

# A push run that does not LIST the suite job at all refuses the same way.
gh_says "$(run_row "$BASE" success)" "$(push_rows success | grep -v 'QEMU')"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "an all-green run WITHOUT the suite job -> refuses" "$?" "1"

# ...and the control: the same run with the suite job green is accepted, so
# the assertions above are about the JOB and not about the run being short.
gh_says "$(run_row "$BASE" success)" "$(job_rows success success)"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "control: the same run WITH the suite job -> accepts" "$?" "0"

# A FULL run needs the cheap jobs too, by name. A dispatched run always has
# them, so this costs nothing -- and it keeps "covered by CI" from meaning
# "the suite passed on a tree that clang-tidy or the aa64 build rejected".
for _j in "gcc x64" "gcc aa64" "clang-tidy"; do
    gh_says "$(run_row "$BASE" success)" "$(job_rows success success | grep -vF -- "$_j"$'\t')"
    release_gate_ci_covers "$BASE" >/dev/null 2>&1
    check "a suite run lacking '$_j' -> refuses" "$?" "1"
done

# TWO RUNS AT ONE COMMIT, and the push run is the newest. The gate used to
# stop at the FIRST green run it listed, so a green push run (no suite) sitting
# above a green dispatched run (suite) refused a commit the suite HAD covered.
# Every green run is examined now.
gh_says "$(printf '%s\n%s' "$(run_row "$BASE" success 5001)" "$(run_row "$BASE" success 5000)")" ""
push_rows success > "$GH_JOBS.5001"
job_rows success success > "$GH_JOBS.5000"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "a push run ABOVE a dispatched suite run at the same commit -> accepts" "$?" "0"
# Control: the same two runs with the dispatched one red -> refuses, so the
# accept above came from run 5000 and not from reading run 5001 as green.
job_rows success failure > "$GH_JOBS.5000"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "control: ...with the dispatched run's suite FAILED -> refuses" "$?" "1"

# The job names are a second spelling of ci.yml's, so hold them equal here --
# same reason as the [release-cut] marker above.
grep -qF "name: $AXL_CI_SUITE_JOB" "$PROJECT_DIR/.github/workflows/ci.yml"
check "the suite job name matches ci.yml" "$?" "0"
check "the cheap job list is exactly the three push jobs" \
    "$(printf '%s|' "${AXL_CI_CHEAP_JOBS[@]}")" "gcc x64|gcc aa64|clang-tidy|"
# `gcc x64`/`gcc aa64` are a matrix expansion, not a literal, so the template
# and both matrix entries are what to hold equal.
grep -qF 'name: gcc ${{ matrix.arch }}' "$PROJECT_DIR/.github/workflows/ci.yml" \
    && grep -qE '^ +- arch: x64$' "$PROJECT_DIR/.github/workflows/ci.yml" \
    && grep -qE '^ +- arch: aa64$' "$PROJECT_DIR/.github/workflows/ci.yml" \
    && grep -qE '^    name: clang-tidy$' "$PROJECT_DIR/.github/workflows/ci.yml"
check "the cheap job names match ci.yml (gcc matrix x64/aa64, clang-tidy)" "$?" "0"

# ── the hole the docs-only skip opened in this very gate ──────────────────
#
# ci-plan.sh makes a prose push skip every job, so after one there is NO green
# run at the tip -- and the gate refused the release, sending the cut back to
# the ~8.5-minute local suite that §13.1 had just removed. The two mechanisms
# fought each other, and the second one shipped first.
#
# Accepting it is consistent rather than lenient: "this diff cannot affect the
# suite" is exactly what ci-plan.sh asserts, and that claim is sabotage-verified
# in test-ci-plan.sh. So the walk back through ancestors allows a diff that is
# release-only files, docs-safe paths, or both -- and nothing else.
CIPARENT2=$(git rev-parse HEAD)
echo prose >> docs/AXL-Design.md 2>/dev/null || { mkdir -p docs; echo prose > docs/AXL-Design.md; }
git add -A >/dev/null; git commit -qm "docs: prose only" >/dev/null
DOCSTIP=$(git rev-parse HEAD)
gh_says "$(run_row "$CIPARENT2" success)" "$(job_rows success success)"
release_gate_ci_covers "$DOCSTIP" >/dev/null 2>&1
check "a green ancestor + a docs-only diff -> accepts" "$?" "0"

# ...and with the version bump on top of the prose, which is the real shape of
# a cut made after a docs push.
echo 9.9.9 > VERSION; echo h9 > include/axl/axl-version.h; echo l9 > CHANGELOG.md
git add -A >/dev/null; git commit -qm "release: v9.9.9" >/dev/null
DOCSREL=$(git rev-parse HEAD)
gh_says "$(run_row "$CIPARENT2" success)" "$(job_rows success success)"
release_gate_ci_covers "$DOCSREL" >/dev/null 2>&1
check "a green ancestor + docs + the version bump -> accepts" "$?" "0"

# THE CONTROL, and it is the whole point: one C file in that range and the
# walk must refuse. Otherwise "walk back until something is green" is just a
# way to tag untested code with extra steps.
echo code >> src.c
git add -A >/dev/null; git commit -qm "and a source change" >/dev/null
DOCSCODE=$(git rev-parse HEAD)
gh_says "$(run_row "$CIPARENT2" success)" "$(job_rows success success)"
release_gate_ci_covers "$DOCSCODE" >/dev/null 2>&1
check "control: a C file anywhere in the range -> refuses" "$?" "1"

# A machine with no gh cannot answer, and MUST NOT answer yes. "The tool could
# not run" and "the tool ran and found nothing" are the same empty string and
# opposite facts.
# Both refuse, but as "could not check" (2), not "not covered" (1).
gh_says "$(run_row "$BASE" success)" "$(job_rows success success)"
AXL_GH="$TMP/no-such-gh" release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "no gh -> could not check (2)" "$?" "2"
touch "$GH_FAIL"
out=$(release_gate_ci_covers "$BASE" 2>&1); rc=$?
check "gh FAILS (expired token) -> could not check (2)" "$rc" "2"
check "...and says so, naming the call that failed" "$out" \
"$(printf "could not check full CI run (with 'QEMU integration (full suite)') for %s:\n  %s: \`gh run list\` failed" "$BASE" "${BASE:0:8}")"
gh_reset
# ...and a jobs listing that cannot be read is "could not check" too, even
# when the run list itself answered.
gh_says "$(run_row "$BASE" success)" "$(job_rows success success)"
printf '#!/bin/bash\n[[ " $* " == *" view "* ]] && exit 1\nexec %q "$@"\n' "$TMP/bin/gh" > "$TMP/gh-view-fails"
chmod +x "$TMP/gh-view-fails"
AXL_GH="$TMP/gh-view-fails" release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "gh run view FAILS -> could not check (2)" "$?" "2"

# The last line of an answer must still count when nothing terminates it.
gh_says_unterminated "$(run_row "$BASE" success)" "$(job_rows success success)"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "an answer with no trailing newline is still read" "$?" "0"

echo ""
echo "=== ci.yml: cheap jobs on push, the suite on dispatch (§14) ==="
# THE POLICY ITSELF, held structurally because a workflow runs only on GitHub:
# no local gate can execute it, and the gate above is only right if the
# workflow really has the shape it assumes.
#
# job_if <job-key> -- that job's own `if:` line (4-space indent), or nothing.
job_if() {
    awk -v j="  $1:" '/^jobs:/{inj=1;next} inj && /^  [A-Za-z][A-Za-z0-9_-]*:[[:space:]]*$/{cur=$0}
         inj && cur==j && /^    if:/{print; exit}' "$CI"
}
DISPATCH_ONLY="github.event_name == 'workflow_dispatch'"
_if=$(job_if integration)
_ok=1; [[ -n "$_if" && "$_if" == *"$DISPATCH_ONLY"* ]] && _ok=0
check "the integration job runs only on workflow_dispatch" "$_ok" "0"
# ...and the cheap jobs did NOT pick the condition up, or a push would run
# nothing at all and the 2026-08-18 class (a clean checkout that does not
# link) would be back to five days undetected.
for _job in plan build lint; do
    _if=$(job_if "$_job")
    _ok=1; [[ -n "$_if" && "$_if" != *"workflow_dispatch"* ]] && _ok=0
    check "the $_job job still runs on push (its if: does not ask for a dispatch)" "$_ok" "0"
done
grep -qE '^  push:$' "$CI"
check "ci.yml still triggers on push" "$?" "0"
# A PUSH MUST NOT CANCEL A DISPATCHED SUITE. The workflow-level concurrency
# group used to be one per ref with cancel-in-progress, so any push to main
# killed a ~9-minute dispatched suite run -- including the one a release cut
# is waiting on. One group per event keeps pushes collapsing among themselves.
grep -qF 'group: ci-${{ github.ref }}-${{ github.event_name }}' "$CI"
check "the run-level concurrency group is per event (a push cannot cancel a dispatch)" "$?" "0"

echo ""
echo "=== check-main-ci.sh: a push run is green without the suite ==="
# Since §14 a push run never contains the suite, so the per-push health check
# asks for the CHEAP jobs. Asking for the suite would report every push NOT
# GREEN, and a check that is always red is read as noise.
CMC="$PROJECT_DIR/scripts/check-main-ci.sh"
gh_says "$(run_row "$BASE" success)" "$(push_rows success)"
out=$("$CMC" "$BASE" 2>&1); rc=$?
check "a green push run -> exit 0" "$rc" "0"
check "...reported as the cheap jobs passing" "$out" \
    "check-main-ci: green -- cheap push CI (gcc x64, gcc aa64, clang-tidy) is green on $BASE"
gh_says "$(run_row "$BASE" success)" "$(push_rows_without clang-tidy)"
"$CMC" "$BASE" >/dev/null 2>&1
check "control: the same run without clang-tidy -> exit 1" "$?" "1"
touch "$GH_FAIL"
"$CMC" "$BASE" >/dev/null 2>&1
check "gh FAILS -> exit 2 (could not check), not MISSING" "$?" "2"
gh_reset

# AN INERT TIP IS ANSWERED BY THE COMMIT BELOW IT (review ruling R-E). A
# docs-only or [release-cut] tip's own run skips the cheap jobs; the walk
# carries the code commit's green across it, as the release gate does.
echo cmc-code > cmc.c; git add -A >/dev/null; git commit -qm "cmc code" >/dev/null
CMCODE=$(git rev-parse HEAD)
mkdir -p docs; echo prose > docs/cmc-note.md
git add -A >/dev/null; git commit -qm "cmc docs" >/dev/null
CMDOCS=$(git rev-parse HEAD)
gh_says "$(printf '%s\n%s' "$(run_row "$CMDOCS" success 7002)" "$(run_row "$CMCODE" success 7001)")" ""
push_rows skipped > "$GH_JOBS.7002"; push_rows success > "$GH_JOBS.7001"
out=$("$CMC" "$CMDOCS" 2>&1); rc=$?
check "an inert (docs) tip over a green code run -> exit 0" "$rc" "0"
check "...naming the commit that answered" "$out" \
    "check-main-ci: green -- cheap push CI (gcc x64, gcc aa64, clang-tidy) is green on $CMCODE, and everything since is prose or the version bump"

# "PENDING" ONLY WHEN THE DECIDING RUN IS PENDING. The docs tip's run still
# going decides nothing -- its cheap jobs will skip -- so a green code run
# below it is GREEN, not pending...
gh_says "$(printf '%s\tin_progress\t\t7002\n%s' "$CMDOCS" "$(run_row "$CMCODE" success 7001)")" ""
push_rows success > "$GH_JOBS.7001"
out=$("$CMC" "$CMDOCS" 2>&1); rc=$?
check "a pending docs-tip run over a green code run -> green, not pending" "$(head -c 21 <<< "$out")" \
    "check-main-ci: green "
# ...while the CODE run still going is what decides, so that is pending.
gh_says "$(printf '%s\tcompleted\tsuccess\t7002\n%s\tin_progress\t\t7001' "$CMDOCS" "$CMCODE")" ""
push_rows skipped > "$GH_JOBS.7002"
out=$("$CMC" "$CMDOCS" 2>&1); rc=$?
check "the deciding (code) run pending -> exit 0" "$rc" "0"
check "...reported as pending" "$(head -1 <<< "$out")" \
    "check-main-ci: pending -- the run that decides $CMDOCS is still going"
gh_reset

echo ""
echo "=== release_gate_decide: the gate ENFORCES by default (R-B) ==="
# cut-release.sh asks this one function, in the normal flow AND on --resume,
# so the refusal cannot be bypassed by the path you happen to take.
gh_says "" ""
rm -f "$AXL_STAMP_FILE"
out=$(release_gate_decide "$CMDOCS" false false main 2>&1); rc=$?
check "nothing covers, no flags -> REFUSES (1)" "$rc" "1"
check "...naming the dispatch remedy" \
    "$(grep -cxF '       gh workflow run ci.yml --ref main' <<< "$out")" "1"
check "...naming the local release-suite remedy" \
    "$(grep -cxF '       ./scripts/release-suite.sh' <<< "$out")" "1"
check "...and the last line says it refuses and names the overrides" "$(tail -1 <<< "$out")" \
    "refusing to tag: --ci-gate dispatches the suite and waits; --tag-untested overrides, and is recorded in the release notes"
release_gate_decide "$CMDOCS" false false main >/dev/null 2>&1
check "...and the decision is 'refuse'" "$RELEASE_GATE_DECISION" "refuse"

out=$(release_gate_decide "$CMDOCS" true false main 2>&1); rc=$?
check "nothing covers, --ci-gate -> proceeds (0)" "$rc" "0"
release_gate_decide "$CMDOCS" true false main >/dev/null 2>&1
check "...and the decision is 'dispatch'" "$RELEASE_GATE_DECISION" "dispatch"

out=$(release_gate_decide "$CMDOCS" false true main 2>&1); rc=$?
check "nothing covers, --tag-untested -> proceeds (0)" "$rc" "0"
check "...with a loud WARNING as its last line" "$(tail -1 <<< "$out")" \
    "WARNING: --tag-untested: tagging $CMDOCS with NOTHING certifying it; this is recorded in the release notes"
release_gate_decide "$CMDOCS" false true main >/dev/null 2>&1
check "...and the decision is 'untested'" "$RELEASE_GATE_DECISION" "untested"
# Both flags: dispatching is the stronger answer, so it wins.
release_gate_decide "$CMDOCS" true true main >/dev/null 2>&1
check "--ci-gate AND --tag-untested -> dispatch wins" "$RELEASE_GATE_DECISION" "dispatch"

# Covered by the local release suite + the cheap push CI -> no flags needed.
gh_says "$(run_row "$CMCODE" success 7001)" "$(push_rows success)"
release_gate_write "$CMCODE" X64
release_gate_decide "$CMCODE" false false main >/dev/null 2>&1; rc=$?
check "covered by the release-suite stamp -> 0" "$rc" "0"
check "...decision 'covered-local'" "$RELEASE_GATE_DECISION" "covered-local"
# Covered by a dispatched run -> 'covered-ci'.
rm -f "$AXL_STAMP_FILE"
gh_says "$(run_row "$CMCODE" success 7001)" "$(job_rows success success)"
release_gate_decide "$CMCODE" false false main >/dev/null 2>&1; rc=$?
check "covered by a dispatched suite run -> 0" "$rc" "0"
check "...decision 'covered-ci'" "$RELEASE_GATE_DECISION" "covered-ci"
gh_reset

# The note --tag-untested records in the CHANGELOG section and the tag.
check "the untested note is exact" "$(release_gate_untested_note "$CMDOCS")" \
    "**Released untested (--tag-untested).** Neither a dispatched CI suite run nor a local scripts/release-suite.sh run certified $CMDOCS when this tag was cut."

echo ""
echo "=== release_gate_wait_push_run: wait, then fail closed (R-C) ==="
# cut-release waits for the push run of the commit it is about to release to
# finish BEFORE pushing the release commit, so the [release-cut] push never
# races it -- and refuses if that run did not succeed.
SEQ="$TMP/gh-seq"; SEQN="$TMP/gh-seq.n"
cat > "$SEQ" <<STUB
#!/bin/bash
# Answers the Nth call with line N of \$GH_SEQ_FILE (the last line repeats).
n=\$(( \$(cat "$SEQN" 2>/dev/null || echo 0) + 1 )); echo "\$n" > "$SEQN"
[[ -s "\$GH_SEQ_FILE" ]] || exit 0
total=\$(wc -l < "\$GH_SEQ_FILE")
(( n > total )) && n=\$total
sed -n "\${n}p" "\$GH_SEQ_FILE"
STUB
chmod +x "$SEQ"
seq_says() { rm -f "$SEQN"; printf '%s\n' "$@" > "$TMP/seq.txt"; }
export GH_SEQ_FILE="$TMP/seq.txt"
W=$CMCODE
seq_says "$(printf '%s\tin_progress\t\t8001' "$W")" "$(printf '%s\tcompleted\tsuccess\t8001' "$W")"
out=$(AXL_GH="$SEQ" AXL_GATE_POLL_SECS=0 release_gate_wait_push_run "$W" 2>&1); rc=$?
check "in progress, then success -> 0" "$rc" "0"
check "...after polling more than once" "$(cat "$SEQN")" "2"
seq_says "$(printf '%s\tcompleted\tfailure\t8001' "$W")"
out=$(AXL_GH="$SEQ" AXL_GATE_POLL_SECS=0 release_gate_wait_push_run "$W" 2>&1); rc=$?
check "a FAILED push run -> refuses (1)" "$rc" "1"
check "...and says so" "$out" "push run 8001 at $W concluded failure -- refusing; fix it and push again"
seq_says ""
out=$(AXL_GH="$SEQ" AXL_GATE_POLL_SECS=0 release_gate_wait_push_run "$W" 2>&1); rc=$?
check "no push run at all -> 0 (the gate decides without one)" "$rc" "0"
check "...and says there was none" "$out" \
    "no push run at $W (not pushed, or the event was dropped) -- the gate decides without one"
seq_says "$(printf '%s\tin_progress\t\t8001' "$W")"
AXL_GH="$SEQ" AXL_GATE_POLL_SECS=0 AXL_GATE_POLL_MAX=3 release_gate_wait_push_run "$W" >/dev/null 2>&1
check "still running at the limit -> 3" "$?" "3"
printf '#!/bin/bash\nexit 1\n' > "$TMP/gh-dead"; chmod +x "$TMP/gh-dead"
AXL_GH="$TMP/gh-dead" AXL_GATE_POLL_SECS=0 release_gate_wait_push_run "$W" >/dev/null 2>&1
check "gh FAILS -> could not check (2)" "$?" "2"
unset GH_SEQ_FILE

echo ""
echo "=== ci.yml: the integration job IS scripts/release-suite.sh (R-A) ==="
# job_block <job-key> -- that job's lines.
job_block() {
    awk -v j="  $1:" '/^jobs:/{inj=1;next} inj && /^  [A-Za-z][A-Za-z0-9_-]*:[[:space:]]*$/{cur=$0}
         inj && cur==j' "$CI"
}
_ib=$(job_block integration | sed 's/#.*//')
check "the integration job runs ./scripts/release-suite.sh --ci" \
    "$(grep -cE 'run: \./scripts/release-suite\.sh --ci$' <<< "$_ib")" "1"
# ...and runs none of its steps itself: one script, one list, or the local
# release run and CI's job certify different things again.
check "...and runs none of its steps directly" \
    "$(grep -cE 'test-axl\.sh|run-integration\.sh|run-selftests\.sh|install\.sh' <<< "$_ib")" "0"
# A [release-cut] push must not cancel its parent's cheap run.
grep -qF "cancel-in-progress: \"\${{ !($GUARD_SUBJ && $GUARD_MARK) }}\"" "$CI"
check "a [release-cut] push does not cancel an in-progress run" "$?" "0"

echo ""
echo "=== cut-release: one gate, enforced, on every path (R-B, R-C) ==="
_cut_code=$(sed 's/#.*//' "$CUT")
check "--tag-untested is a flag" "$(grep -cE '^ +--tag-untested\) +TAG_UNTESTED=true' <<< "$_cut_code")" "1"
# The normal flow decides BEFORE it bumps anything, so a refusal leaves no
# release commit stranded on main.
_dec=$(grep -n 'release_gate_decide "\$(git rev-parse HEAD)"' <<< "$_cut_code" | head -1 | cut -d: -f1)
_bump=$(grep -n '^scripts/bump-version.sh "\$VERSION"' <<< "$_cut_code" | head -1 | cut -d: -f1)
_ok=1; [[ -n "$_dec" && -n "$_bump" && "$_dec" -lt "$_bump" ]] && _ok=0
check "the normal flow decides the gate before the version bump" "$_ok" "0"
_wait=$(grep -n 'release_gate_wait_push_run "\$(git rev-parse HEAD)"' <<< "$_cut_code" | head -1 | cut -d: -f1)
_ok=1; [[ -n "$_wait" && -n "$_dec" && "$_wait" -lt "$_dec" ]] && _ok=0
check "...after waiting for HEAD's push run" "$_ok" "0"
_resume=$(awk '/^if \$RESUME; then$/{f=1} f{print} f&&/^fi$/{exit}' "$CUT" | sed 's/#.*//')
check "--resume goes through the same gate" "$(grep -c 'release_gate_decide' <<< "$_resume")" "1"
check "a refusal exits non-zero (both paths)" \
    "$(grep -cF '[[ "$RELEASE_GATE_DECISION" != refuse ]] || exit 1' <<< "$_cut_code")" "2"
check "the untested note is made once (the normal flow)" \
    "$(grep -c 'release_gate_untested_note' <<< "$_cut_code")" "1"
# Recorded through the shared helper, and the cut DIES if it did not land.
check "the note is recorded by release_gate_record_untested, or the cut dies" \
    "$(grep -v '^ *#' "$CUT" | sed -e ':a' -e '/\\$/{N;s/\\\n//;ba' -e '}' \
       | grep -cE 'release_gate_record_untested CHANGELOG\.md .*\|\| die ')" "1"
check "...and the tag message carries it" \
    "$(awk '/^make_tag_message\(\) \{/{f=1} f{print} f&&/^\}$/{exit}' "$CUT" | grep -c 'UNTESTED_NOTE')" "1"

echo ""
echo "=== --tag-untested refuses --resume ==="
# On --resume the release commit is already pushed, and release.yml builds the
# release notes from its CHANGELOG section ALONE -- so the note could reach the
# tag message but not the notes a consumer reads. Refused at parse time.
out=$("$CUT" not-a-version --resume --tag-untested 2>&1); rc=$?
# The exit status alone cannot discriminate here: the invalid version exits 2
# too. The MESSAGE is what proves the combination was refused.
check "--resume --tag-untested -> 2" "$rc" "2"
check "--resume --tag-untested: refused for the combination, saying why" "$(sed -n 1p <<< "$out")" \
    "ERROR: --tag-untested cannot be used with --resume: the release commit is already pushed, and release.yml builds the release notes from its CHANGELOG section alone, so the note could not reach them."
check "...and how to proceed" "$(sed -n 2p <<< "$out")" \
    "       Certify the commit instead: --resume --ci-gate dispatches the suite and waits; or run scripts/release-suite.sh on it, then --resume."
# Control: each flag ALONE is not refused for the combination. Driven with an
# invalid version, so the run stops at argument validation (exit 2) whatever
# the tree looks like -- a valid one would walk into the real preconditions.
for _f in --resume --tag-untested; do
    out=$("$CUT" not-a-version "$_f" 2>&1)
    check "control: $_f alone is not refused for the combination" \
        "$(grep -c 'cannot be used with --resume' <<< "$out")" "0"
done

echo ""
echo "=== the untested note reaches the PUBLISHED notes (fixture) ==="
# The real path, end to end on a sample file: cut-release's insertion
# (release_gate_record_untested), then release.yml's OWN extraction
# (scripts/changelog-section.sh, which the workflow calls -- not a copy).
CLF="$TMP/CHANGELOG.md"
printf '# Changelog\n\n## 9.9.9 — 2026-10-21\n\n### Fixed\n\n- **a fix.**\n\n## 9.9.8 — 2026-10-01\n\n- old\n' > "$CLF"
NOTE=$(release_gate_untested_note "$CMDOCS")
release_gate_record_untested "$CLF" "## 9.9.9 — 2026-10-21" "$NOTE"
check "recording the note succeeds" "$?" "0"
SECTION=$("$PROJECT_DIR/scripts/changelog-section.sh" 9.9.9 "$CLF"); rc=$?
check "release.yml's extraction finds the section" "$rc" "0"
check "...and the note is IN the published notes" "$(grep -cxF -- "$NOTE" <<< "$SECTION")" "1"
check "...and nothing leaks in from the previous version" "$(grep -c 'old' <<< "$SECTION")" "0"
# Control: a heading the file does not have -> the note did not land -> 1, so
# cut-release's `|| die` has something to see.
cp "$CLF" "$TMP/CL2.md"
printf '# Changelog\n\n## 9.9.9 — 2026-10-21\n\n- x\n' > "$TMP/CL2.md"
release_gate_record_untested "$TMP/CL2.md" "## 1.2.3 — 2000-01-01" "$NOTE"
check "control: a heading that is not there -> 1 (the note did not land)" "$?" "1"
check "release.yml builds the body with scripts/release-notes.sh (one builder, not two)" \
    "$(grep -cF 'scripts/release-notes.sh "$V"' "$PROJECT_DIR/.github/workflows/release.yml")" "1"
check "...which extracts with scripts/changelog-section.sh (one extraction, not two)" \
    "$(grep -cF '/changelog-section.sh" "$V"' "$PROJECT_DIR/scripts/release-notes.sh")" "1"
"$PROJECT_DIR/scripts/changelog-section.sh" 1.0.0 "$CLF" >/dev/null 2>&1
check "control: an absent version -> the extraction fails (1)" "$?" "1"

echo ""
echo "=== the release body fits what GitHub accepts ==="
# v5.0.0's first publish failed: its section was 228 KB, passed INLINE to the
# publish action -- over Linux's 128 KB limit on one argument or environment
# string ("Argument list too long"), and over GitHub's own 125,000-character
# cap on a release body. release.yml now hands the action a FILE
# (body_path:) that scripts/release-notes.sh builds and caps.
NOTES="$PROJECT_DIR/scripts/release-notes.sh"
FOOTER='📦 **Install / quick start:** see the [README on the releases repo](https://github.com/aximcode/axl-sdk-releases#install-the-sdk).
📚 **API reference:** [axl.aximcode.com](https://axl.aximcode.com/).
🔒 **Verify downloads:** [`SHA256SUMS`](https://github.com/aximcode/axl-sdk-releases/releases/download/v9.9.9/SHA256SUMS).
📂 **Source archive:** [`v9.9.9.tar.gz`](https://github.com/aximcode/axl-sdk-releases/archive/refs/tags/v9.9.9.tar.gz).'
TAIL=$'\n\n---\n\n'"$FOOTER"
printf '# Changelog\n\n## 9.9.9 — 2026-11-10\n\n### Fixed\n\n- **a fix.**\n\n## 9.9.8\n\n- old\n' > "$TMP/small.md"
OUT=$("$NOTES" 9.9.9 "$TMP/small.md"); rc=$?
check "a short section: exit 0" "$rc" "0"
check "a short section: the body is the section, a rule and the footer, exactly" \
    "$OUT" $'### Fixed\n\n- **a fix.**'"$TAIL"
"$NOTES" 1.0.0 "$TMP/small.md" >/dev/null 2>&1
check "an absent version: the extraction's 1 comes through" "$?" "1"

# A long section, multi-byte throughout (an e-acute is 2 bytes, 1 character):
# the cap is counted in BYTES, which can only over-count GitHub's characters.
{
    printf '# Changelog\n\n## 9.9.9 — 2026-11-10\n\n### Breaking\n\n'
    _pad=$(printf 'é%.0s' {1..100})
    for _i in $(seq 1 1500); do
        printf -- '- **entry %d.** %s\n  continued %d.\n\n' "$_i" "$_pad" "$_i"
    done
    printf '## 9.9.8\n\n- old\n'
} > "$TMP/big.md"
"$NOTES" 9.9.9 "$TMP/big.md" > "$TMP/big.out"; rc=$?
check "a long section: exit 0" "$rc" "0"
_bytes=$(wc -c < "$TMP/big.out")
check "a long section: the body is at most 120000 bytes" \
    "$(( _bytes <= 120000 ))" "1"
check "...and not cut more than one entry short (over 119500 bytes)" \
    "$(( _bytes > 119500 ))" "1"
_k=$(grep -c '^- \*\*entry ' "$TMP/big.out")
check "...and the kept entries are the FIRST ones, whole and in order" \
    "$(grep '^- \*\*entry ' "$TMP/big.out" | sed 's/^- \*\*entry \([0-9]*\)\..*/\1/' | tr '\n' ' ')" \
    "$(seq -s ' ' 1 "$_k") "
check "...the last kept entry keeps its continuation line" \
    "$(grep -c "^  continued ${_k}\.\$" "$TMP/big.out")" "1"
_note="*These notes stop after ${_k} of 1500 entries: a GitHub release holds at most 125,000 characters. Every entry is in [CHANGELOG.md](https://github.com/aximcode/axl-sdk-releases/blob/v9.9.9/CHANGELOG.md).*"
check "...it ends with the cut note and the footer, exactly" \
    "$(tail -c "$(( ${#_note} * 4 + 2000 ))" "$TMP/big.out" | awk -v n="$_note" 'index($0, n) == 1 { f = 1 } f' | head -c 100000)" \
    "$_note$TAIL"
check "...nothing leaks in from the previous version" \
    "$(grep -cx -- '- old' "$TMP/big.out")" "0"

# The boundary, byte-exact: a body of exactly 120000 bytes is published whole;
# one byte more is cut. The overhead is measured, not restated.
_one() {   # $1 = pad length -> a one-entry section
    printf '# Changelog\n\n## 9.9.9 — 2026-11-10\n\n### Fixed\n\n- **x.** %s\n' \
        "$(head -c "$1" /dev/zero | tr '\0' 'a')" > "$TMP/one.md"
    "$NOTES" 9.9.9 "$TMP/one.md" > "$TMP/one.out"
}
_one 0; _over=$(wc -c < "$TMP/one.out")
_one $(( 120000 - _over ))
check "exactly 120000 bytes: published whole" \
    "$(wc -c < "$TMP/one.out") $(grep -c 'These notes stop' "$TMP/one.out")" "120000 0"
_one $(( 120001 - _over ))
check "120001 bytes: cut, and says so" \
    "$(grep -c 'These notes stop after 0 of 1 entries' "$TMP/one.out")" "1"
check "...still within the cap" "$(( $(wc -c < "$TMP/one.out") <= 120000 ))" "1"

# A line inside a code fence is not an entry boundary, even when it starts
# `- ` or `#` at column 0: a cut there leaves the fence open, and the note and
# footer render as code. Fixture: one entry holding a long fenced listing of
# `- ` lines that straddles the cap, then a few more entries.
{
    printf '# Changelog\n\n## 9.9.9 — 2026-11-10\n\n### Fixed\n\n- **fenced.**\n\n```\n'
    for _i in $(seq 1 2000); do printf -- '- listing line %d %s\n' "$_i" "$(printf 'x%.0s' {1..60})"; done
    printf '# a shell comment\n```\n\n- **after one.**\n\n- **after two.**\n'
} > "$TMP/fence.md"
"$NOTES" 9.9.9 "$TMP/fence.md" > "$TMP/fence.out"; rc=$?
check "a fenced listing past the cap: exit 0" "$rc" "0"
check "...no fence is left open (an even count of fence lines)" \
    "$(( $(grep -c '^```' "$TMP/fence.out") % 2 ))" "0"
check "...the cut falls before the fenced entry: nothing kept, says so" \
    "$(grep -c '^\*These notes stop after 0 of 3 entries: ' "$TMP/fence.out")" "1"
check "...within the cap" "$(( $(wc -c < "$TMP/fence.out") <= 120000 ))" "1"

echo ""
echo "=== cut-release's header describes the current flow ==="
_hdr=$(sed -n '1,/^set -euo pipefail/p' "$CUT")
check "the header names scripts/release-suite.sh as the local remedy" \
    "$(grep -q 'scripts/release-suite.sh' <<< "$_hdr" && echo yes)" "yes"
check "...and no longer advises run-integration.sh --no-cache" \
    "$(grep -c 'run-integration.sh --no-cache' <<< "$_hdr")" "0"
check "the stamp helper is not described as shared with run-integration.sh" \
    "$(grep -c 'shared with run-integration.sh' "$CUT")" "0"

echo ""
echo "=== cut-release: the watch is opt-in ==="
# SCOPE NOTE. This file is about the machinery that decides whether a release
# is verified, and the watch is not that -- it decides whether a HUMAN waits.
# It lives here because the structural assertions about ci.yml/release.yml
# above already made this file the home for cut-release's contract, and two
# assertions do not earn a file of their own.
#
# WHAT IS AND IS NOT PINNED, said plainly rather than implied: the watcher
# itself is covered by test-release-watch-scope.sh and is unchanged. What
# changed is which path cut-release takes by default, and the end-to-end proof
# of that is the next real cut -- there is no harness here that can push a tag.
# So this pins the two things that are checkable without one.
CUTREL="$PROJECT_DIR/scripts/cut-release.sh"

# The flag is KNOWN. Argument parsing runs before every precondition, so an
# unknown flag exits 2 at parse time and a known one gets past it to fail on
# something else -- which is what distinguishes the two without a clean tree,
# a tag, or a network.
out=$("$CUTREL" 9.9.9 --watch 2>&1); rc=$?
if [[ "$out" != *"unknown flag"* ]]; then
    pass "--watch is a known flag"
else
    fail "--watch was rejected as unknown (rc=$rc): $out"
fi
out=$("$CUTREL" 9.9.9 --nonsense-flag 2>&1); rc=$?
if [[ $rc -eq 2 && "$out" == *"unknown flag"* ]]; then
    pass "control: an unknown flag still exits 2, so the check above discriminates"
else
    fail "an unknown flag was accepted (rc=$rc): $out"
fi

# THE MEASURED WORKER COUNT, pinned as an exact string. cut-release.sh advised
# `-j$(nproc)` in the branch it printed after every cut, and RELEASING.md
# deleted that advice on 2026-09-02 with a measurement: on 8 cores it failed
# test-task-pool-mp-qemu.sh twice ("slot reclaim: workers=2 expected=3") while
# the default nproc-2 was green -- 474s against 477s. Three seconds, for a
# false red on the one gate that decides whether to publish. The script was
# then the last place still recommending it.
# COMMENTS STRIPPED FIRST, the same treatment the [skip ci] check above needs
# and for the identical reason it records: the comment explaining why we do NOT
# advise this contains the string, so a raw scan finds its own rationale. "A
# check that cannot survive its own subject being discussed" -- third instance
# today, after the ci.yml skip guard and this file's own earlier one.
if sed 's/#.*//' "$CUTREL" | grep -qF -- '-j\$(nproc)'; then
    fail "cut-release.sh still advises -j\$(nproc), which RELEASING.md measured into a false red"
    grep -nF -- '-j\$(nproc)' "$CUTREL" | grep -v '^ *#' | sed 's/^/      /'
else
    pass "cut-release.sh does not advise the worker count that produced a false red"
fi

# ...and it must not tell you the local suite is the gate right after telling
# you CI satisfied it. The v4.7.1 cut printed both, four lines apart -- the
# same self-contradicting-output defect fixed in `axl prune` the same day. The
# note is now inside a branch that only runs when nothing covered the commit.
if sed 's/#.*//' "$CUTREL" | grep -qF 'the LOCAL suite is the release gate now'; then
    fail "the unconditional 'LOCAL suite is the gate' note is back; it contradicts the gate line above it"
else
    pass "no unconditional note claiming the local suite is the gate"
fi

# The default path must hand back the verification command, because that is
# what replaces the wait. A cut that neither waits nor says how to check is
# strictly worse than the one being replaced.
if grep -qF 'scripts/check-published-release.sh $TAG' "$CUTREL"; then
    pass "the non-watching path names check-published-release.sh"
else
    fail "the non-watching path does not hand back a way to verify the release"
fi

echo "release-gate: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
