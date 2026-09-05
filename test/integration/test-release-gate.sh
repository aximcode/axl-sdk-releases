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
# The CI half of the gate asks GitHub whether a run went green at a commit.
# Driven through a stub so it needs no network and no real run, and so the
# REFUSING cases -- which are the ones that matter, since a false positive
# tags code no suite ever ran -- can be produced on demand.
#
# THE STUB ANSWERS WHAT `gh --jq` WOULD PRINT, not raw JSON, and the decisions
# are made in shell rather than inside a jq expression -- so every one of them
# is reachable from here. `gh` embeds its own jq, so this adds no dependency.
mkdir -p "$TMP/bin"
GH_RUNS="$TMP/gh-runs.tsv"
GH_JOBS="$TMP/gh-jobs.txt"
cat > "$TMP/bin/gh" <<STUB
#!/bin/bash
# Two queries are made: the run list for a commit (TSV), and one run's job
# conclusions (one per line).
for a in "\$@"; do
    case "\$a" in
        view) cat "$GH_JOBS"; exit 0 ;;
    esac
done
cat "$GH_RUNS"
STUB
chmod +x "$TMP/bin/gh"

# gh_says <run-tsv-lines> <job-tsv-lines> -- newline-terminated, the way a
# real `gh --jq` writes them. A job line is `<name>\t<conclusion>`: the NAME is
# now part of the answer, because "every job passed" and "the suite ran" became
# different claims the moment a push could produce a partial run.
gh_says() { printf '%s\n' "$1" > "$GH_RUNS"; printf '%s\n' "$2" > "$GH_JOBS"; }
# ...and the same without the final newline, which is what a truncated or
# differently-buffered answer looks like. `read` returns non-zero on such a
# line while still assigning it, so a bare `while read` drops it -- and here
# that line IS the answer.
gh_says_unterminated() { printf '%s' "$1" > "$GH_RUNS"; printf '%s' "$2" > "$GH_JOBS"; }
run_row() { printf '%s\tcompleted\t%s\t4242' "$1" "$2"; }   # <sha> <conclusion>
# job_rows <build-conclusion> <suite-conclusion>
job_rows() { printf 'gcc x64\t%s\nQEMU integration (full suite)\t%s' "$1" "$2"; }

echo "=== release-gate ==="
echo "=== release-gate ==="

release_gate_covers "$BASE" >/dev/null 2>&1
check "no stamp -> refuses" "$?" "1"

release_gate_write "$BASE" X64 159 0
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp on HEAD -> accepts" "$?" "0"

release_gate_write "$BASE" X64 158 1
release_gate_covers "$BASE" >/dev/null 2>&1
check "stamp from a FAILED run -> refuses" "$?" "1"

printf 'sha=\narch=X64\n' > "$AXL_STAMP_FILE"
release_gate_covers "$BASE" >/dev/null 2>&1
check "malformed stamp -> refuses" "$?" "1"

# The release commit: only the three version files change.
release_gate_write "$BASE" X64 159 0
echo 4.2.0 > VERSION; echo hdr2 > include/axl/axl-version.h; echo log2 > CHANGELOG.md
git add -A >/dev/null; git commit -qm "release: v4.2.0"
REL=$(git rev-parse HEAD)
release_gate_covers "$REL" >/dev/null 2>&1
check "release commit over a stamped parent -> accepts" "$?" "0"

# The case that must never pass: real code changed after the stamp.
echo changed > src.c; git add -A >/dev/null; git commit -qm "a real change"
release_gate_covers "$(git rev-parse HEAD)" >/dev/null 2>&1
check "source changed since the stamp -> refuses" "$?" "1"

# A stamp for a commit that is not an ancestor at all.
git checkout -q -b other "$BASE"; echo x > other.c
git add -A >/dev/null; git commit -qm other
release_gate_write "$(git rev-parse HEAD)" X64 159 0
release_gate_covers "$REL" >/dev/null 2>&1
check "stamp from an unrelated commit -> refuses" "$?" "1"

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
[[ "$n_guard" -eq "$n_jobs" && "$n_guard" -gt 0 ]]
check "every ci.yml job is guarded by it ($n_guard of $n_jobs)" "$?" "0"

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
# WHY THIS HALF EXISTS. CI runs the identical uncached suite on every push to
# main, on the same box, and goes green on the commit a release is cut from --
# and the stamp half above cannot see any of that, because it reads a local
# file. So the only way to satisfy the gate was to run the suite a second time
# locally (~8.5 min, blocking a human), and `--ci-gate` would then dispatch a
# THIRD run rather than consult the green one that already exists.

export PATH="$TMP/bin:$PATH"

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

# THE SHAPE THE DOCS SPLIT CREATES, and the reason the gate had to learn job
# NAMES. A docs-only push runs the docs job and nothing else; every job in that
# run succeeds. "All jobs passed" and "the suite ran" stopped being the same
# claim, and reading the first as the second tags code no suite ever touched --
# reachable by cutting a release straight after a README fix.
gh_says "$(run_row "$BASE" success)" "$(printf 'docs\tsuccess')"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "an all-green run WITHOUT the suite job -> refuses" "$?" "1"

# ...and the control: the same run with the suite job present is accepted, so
# the assertion above is about the JOB and not about the run being short.
gh_says "$(run_row "$BASE" success)" "$(printf 'docs\tsuccess\nQEMU integration (full suite)\tsuccess')"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "control: the same run WITH the suite job -> accepts" "$?" "0"

# The job name is a second spelling of ci.yml's, so hold them equal here --
# same reason as the [release-cut] marker above.
grep -qF "$AXL_CI_SUITE_JOB" "$PROJECT_DIR/.github/workflows/ci.yml"
check "the suite job name matches ci.yml" "$?" "0"

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
( PATH="/nonexistent"; release_gate_ci_covers "$BASE" >/dev/null 2>&1 )
check "no gh on PATH -> refuses" "$?" "1"

# The last line of an answer must still count when nothing terminates it.
gh_says_unterminated "$(run_row "$BASE" success)" "$(printf 'QEMU integration (full suite)\tsuccess')"
release_gate_ci_covers "$BASE" >/dev/null 2>&1
check "an answer with no trailing newline is still read" "$?" "0"

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
