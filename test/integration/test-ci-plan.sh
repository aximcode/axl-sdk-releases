#!/bin/bash
# test-meta: arch=x64 needs= est=4 local-only=0
# test-ci-plan.sh — the classifier that decides whether a push runs the suite.
#
# WHY EVERY CASE HERE IS ABOUT REFUSING TO SAY "docs only". A path wrongly put
# in the safe set skips the build, the unit suite and ~180 integration tests
# for a change that could break any of them, and it does it silently -- the run
# is green because the jobs never ran. The other direction costs a run nobody
# needed. So the fail-safe is `docs_only=false`, and the cases below are mostly
# "this looks like prose and is not".
#
# The list it encodes was paid for once already: AXL-CI-Release-Speed-Design.md
# §11.6 rejected `'**/*.md'` for exactly these traps, and its warning is the
# one to keep -- "prose is not a synonym for nothing depends on it".
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

PLAN="$PROJECT_DIR/scripts/ci-plan.sh"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

REPO="$TMP/repo"; mkdir -p "$REPO"; cd "$REPO"
git init -q .; git config user.email t@t; git config user.name t
mkdir -p docs/sphinx/modules docs/notes src/gfx src/mem include/axl
echo root      > README.md
echo design    > docs/AXL-Design.md
echo handoff   > docs/notes/handoff.md
echo rst       > docs/sphinx/index.rst
echo gfxreadme > src/gfx/README.md
echo memreadme > src/mem/README.md
echo code      > src/gfx/axl-gfx.c
echo hdr       > include/axl/axl-gfx.h
git add -A >/dev/null; git commit -qm base
BASE=$(git rev-parse HEAD)

# plan_after <label> <expect-docs_only> <expect-docs_touched> -- commits
# whatever the caller staged, classifies BASE..HEAD, then resets back to BASE
# so each case is independent of the last.
plan_after() {
    local label=$1 want_only=$2 want_touched=$3 out only touched
    git add -A >/dev/null; git commit -qm "$label" >/dev/null
    out=$("$PLAN" "$BASE" HEAD)
    only=$(sed -n 's/^docs_only=//p'    <<< "$out")
    touched=$(sed -n 's/^docs_touched=//p' <<< "$out")
    if [[ "$only" == "$want_only" && "$touched" == "$want_touched" ]]; then
        pass "$label -> docs_only=$only docs_touched=$touched"
    else
        fail "$label -> docs_only=$only docs_touched=$touched (want $want_only / $want_touched)"
    fi
    git reset -q --hard "$BASE"
}

echo "=== ci-plan ==="
echo ""

# ── the safe set ──────────────────────────────────────────────────────────
echo change >> docs/AXL-Design.md
plan_after "a design doc alone" true true

echo change >> docs/notes/handoff.md
plan_after "a nested docs/*.md alone" true true

echo change >> src/gfx/README.md
plan_after "one module README alone" true true

echo a >> src/gfx/README.md; echo b >> src/mem/README.md
plan_after "two module READMEs" true true

# ── the traps §11.6 paid for ──────────────────────────────────────────────
#
# The ROOT README is prose that THREE things read: Sphinx includes it,
# check-tool-docs.py asserts every shipped tool has a row in its table, and
# test-toolchain-variant.sh asserts it documents no `make CROSS=` build. Two of
# those are tests. It feeds the doc build AND must never skip the suite.
echo change >> README.md
plan_after "the ROOT README is not safe" false true

# docs/sphinx/** is read by check-doc-coverage.py, which runs in the lint job.
echo change >> docs/sphinx/index.rst
plan_after "an .rst is not safe" false true

# ── plain code ────────────────────────────────────────────────────────────
echo change >> src/gfx/axl-gfx.c
plan_after "a C file" false false

echo change >> include/axl/axl-gfx.h
plan_after "a public header" false false

# A doc comment lives in a header, so a "documentation" change can be code.
echo a >> src/gfx/README.md; echo b >> src/gfx/axl-gfx.c
plan_after "a README beside code is NOT docs-only" false true

# ── the shapes that must fail safe ────────────────────────────────────────
#
# A DELETION IS NEVER SAFE. test-source-snapshot.sh asserts the published
# snapshot CONTAINS docs/AXL-Design.md -- editing it cannot break that,
# removing it can, and both look identical in --name-only.
git rm -q docs/AXL-Design.md
plan_after "a DELETED design doc is not docs-only" false true

# A brand-new top-level path is unrecognised, and unrecognised means run
# everything: the allowlist cannot have anticipated it.
echo new > NEWTHING.md
plan_after "an unrecognised .md at the root" false false

# An empty diff is not "nothing to do" -- it is a question that was not
# answered, and the two look the same.
out=$("$PLAN" "$BASE" "$BASE")
if [[ "$out" == "docs_only=false"* ]]; then
    pass "an empty diff -> docs_only=false"
else
    fail "an empty diff -> $out"
fi

# ...and a diff that cannot be computed at all must not read as "no docs
# changed, nothing to run".
out=$("$PLAN" "$BASE" "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" 2>/dev/null)
if [[ "$out" == "docs_only=false"* ]]; then
    pass "an uncomputable diff -> docs_only=false"
else
    fail "an uncomputable diff -> $out"
fi

# Missing arguments are a usage error, not a silent full run: a workflow that
# calls this wrong must fail loudly rather than quietly run everything forever.
"$PLAN" >/dev/null 2>&1
if [[ $? -eq 2 ]]; then
    pass "no arguments -> exit 2"
else
    fail "no arguments did not exit 2"
fi

# ── the ci-only class ─────────────────────────────────────────────────────
#
# WHY THIS CLASS IS DEFENSIBLE WHERE "shell changed" IS NOT. §12.5's warning is
# that a relevance map guesses which tests a change can reach, and guesses
# wrong silently -- src/log/ plus the Makefile looked like "the logging tests"
# and the blast radius was every image, failing at link. A `.github/` change is
# different in kind, not degree: NOTHING BUILDS FROM IT. There is no path from
# a .yml file to a produced artifact, so the only things it can reach are CI
# itself and the tests that READ those files.
#
# And that set is DERIVED by grep, not written down here -- a hand-kept list
# would go stale the day a new test starts reading a workflow, which is the
# exact defect this file exists to prevent one level up. Over-inclusion is the
# safe direction: a test that merely mentions .github in a comment gets run.
plan_after_ci() {   # <label> <expect-ci_only>
    local label=$1 want=$2 out only
    git add -A >/dev/null; git commit -qm "$label" >/dev/null
    out=$("$PLAN" "$BASE" HEAD)
    only=$(sed -n 's/^ci_only=//p' <<< "$out")
    if [[ "$only" == "$want" ]]; then
        pass "$label -> ci_only=$only"
    else
        fail "$label -> ci_only=$only (want $want)"
    fi
    git reset -q --hard "$BASE"
}
mkdir -p .github/workflows && echo wf > .github/workflows/ci.yml
git add -A >/dev/null; git commit -qm "add a workflow" >/dev/null; BASE=$(git rev-parse HEAD)

echo change >> .github/workflows/ci.yml
plan_after_ci "a workflow file alone" true

echo a >> .github/workflows/ci.yml; echo b >> src/gfx/axl-gfx.c
plan_after_ci "a workflow beside C is NOT ci-only" false

echo change >> docs/AXL-Design.md
plan_after_ci "a doc alone is not ci-only" false

# The derived list must name tests that EXIST. --only refuses an unknown name,
# so a stale derivation turns into a red CI run rather than a silent skip --
# but catching it here is cheaper than catching it there.
echo change >> .github/workflows/ci.yml
git add -A >/dev/null; git commit -qm "for the list" >/dev/null
listed=$("$PLAN" "$BASE" HEAD | sed -n 's/^only_tests=//p')
git reset -q --hard "$BASE"
if [[ -n "$listed" ]]; then
    pass "ci-only derives a non-empty test list ($listed)"
else
    fail "ci-only derived no tests -- an empty list must fall back to the full run"
fi
missing=""
IFS=',' read -r -a _l <<< "$listed"
for _t in "${_l[@]}"; do
    [[ -f "$PROJECT_DIR/test/integration/$_t" ]] || missing="$missing $_t"
done
if [[ -z "$missing" ]]; then
    pass "every derived test name exists"
else
    fail "derived names that do not exist:$missing"
fi
# It must name the test that actually asserts on ci.yml's contents, or the
# class is skipping the one thing that could catch a bad workflow edit.
if [[ ",$listed," == *",test-release-gate.sh,"* ]]; then
    pass "the derived list includes test-release-gate.sh, which asserts on ci.yml"
else
    fail "the derived list omits test-release-gate.sh: $listed"
fi

# ── the workflow files parse, and parse UNAMBIGUOUSLY ─────────────────────
#
# A DUPLICATE KEY IS NOT A SYNTAX ERROR. YAML takes the last one and
# `yaml.safe_load` accepts it silently, so a "valid YAML" check reports clean
# while a key has been overwritten. That happened here: gating the jobs on
# `plan` added a second `needs:` to two jobs that already had one, and the
# integration job quietly stopped depending on the build. The parse was clean
# both before and after the fix -- only this distinguishes them.
cd "$PROJECT_DIR"
for wf in .github/workflows/*.yml; do
    if python3 - "$wf" <<'PYEOF'
import sys, yaml
class D(yaml.SafeLoader): pass
def nodup(loader, node, deep=False):
    seen = {}
    for k, v in node.value:
        key = loader.construct_object(k, deep=deep)
        if key in seen:
            print(f"duplicate key {key!r}", file=sys.stderr)
            sys.exit(1)
        seen[key] = loader.construct_object(v, deep=deep)
    return seen
D.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, nodup)
yaml.load(open(sys.argv[1]), D)
PYEOF
    then
        pass "$(basename "$wf") parses with no duplicate keys"
    else
        fail "$(basename "$wf") has a duplicate key -- one of them is being silently dropped"
    fi
done

echo ""
echo "ci-plan: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
