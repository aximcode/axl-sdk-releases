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
# THE ci-only CLASS IS GONE (AXL-CI-Release-Speed-Design.md §14). It shrank
# a push run of the integration job, which no longer runs on a push. Held
# here so it cannot come back as an output nothing reads: a field nobody
# consumes is exactly the shape that hid it was never wired into ci.yml.
mkdir -p .github/workflows && echo wf > .github/workflows/ci.yml
git add -A >/dev/null; git commit -qm "add a workflow" >/dev/null; BASE=$(git rev-parse HEAD)
echo change >> .github/workflows/ci.yml
git add -A >/dev/null; git commit -qm "workflow only" >/dev/null
out=$("$PLAN" "$BASE" HEAD)
git reset -q --hard "$BASE"
if [[ "$out" == $'docs_only=false\ndocs_touched=false' ]]; then
    pass "a workflow-only push plans the cheap jobs, and only two outputs exist"
else
    fail "unexpected plan for a workflow-only push: $(tr '\n' ' ' <<< "$out")"
fi

# ── every plan output a job reads is one the plan job EXPORTS ─────────────
#
# A job reading `needs.plan.outputs.X` for an X the plan job's `outputs:` does
# not declare gets the EMPTY STRING, not an error. That is how the ci-only
# class (§13.12) shipped dead: ci-plan.sh emitted ci_only, three steps tested
# `!= 'true'`, and the plan job never exported it -- so every push ran the
# whole suite while the design doc called the class SHIPPED. Held both ways:
# what is read must be exported, and what is exported must be what the script
# emits.
_ci="$PROJECT_DIR/.github/workflows/ci.yml"
_exported=$(awk '/^  plan:/{p=1;next} p&&/^  [A-Za-z]/{p=0}
                 p&&/^    outputs:/{o=1;next} o&&!/^      /{o=0}
                 o&&/^      [a-z_]+:/{sub(/^ +/,"");sub(/:.*/,"");print}' "$_ci" | sort -u)
_read=$(grep -o 'needs\.plan\.outputs\.[a-z_]*' "$_ci" | sed 's/.*\.//' | sort -u)
_emitted=$(cd "$PROJECT_DIR" && "$PLAN" HEAD HEAD | sed 's/=.*//' | sort -u)
_unexported=$(comm -23 <(printf '%s\n' "$_read") <(printf '%s\n' "$_exported"))
if [[ -n "$_exported" && -n "$_read" && -z "$_unexported" ]]; then
    pass "every plan output ci.yml reads is exported ($(tr '\n' ' ' <<< "$_read"))"
else
    fail "ci.yml reads plan outputs the plan job does not export: '$_unexported' (exported: $(tr '\n' ' ' <<< "$_exported"))"
fi
if [[ "$_exported" == "$_emitted" ]]; then
    pass "the plan job exports exactly what ci-plan.sh emits"
else
    fail "plan exports '$(tr '\n' ' ' <<< "$_exported")' but ci-plan.sh emits '$(tr '\n' ' ' <<< "$_emitted")'"
fi

# ── the workflow files: unambiguous, every job bounded, every apt bounded ──
#
# scripts/check-workflows.py holds all four rules and says why each exists
# (a duplicate `needs:` that silently dropped a dependency; a clang-tidy job
# that held the only runner slot for 19 minutes on a dead mirror). This was an
# embedded heredoc doing rule 1 alone until 2026-11-09.
CHECK_WF="$PROJECT_DIR/scripts/check-workflows.py"
cd "$PROJECT_DIR"
_out=$("$CHECK_WF" 2>&1); _rc=$?
# It must have read EVERY workflow, and the job and apt counts must be
# NONZERO: "no jobs found, so nothing unbounded" is the answer a broken parse
# would give.
_n_wf=$(find .github/workflows -maxdepth 1 \( -name '*.yml' -o -name '*.yaml' \) | wc -l)
if [[ "$_rc" -eq 0 && "$_n_wf" -gt 0 && "$_out" =~ ^check-workflows:\ ${_n_wf}\ workflows,\ [1-9][0-9]*\ jobs\ bounded,\ [1-9][0-9]*\ apt-get\ calls\ bounded,\ [1-9][0-9]*\ suite\ jobs\ keep\ logs,\ [1-9][0-9]*\ container\ jobs\ trust\ the\ checkout$ ]]; then
    pass "the workflows: no duplicate key, every job and apt-get bounded ($_out)"
else
    fail "check-workflows.py (exit $_rc): $_out"
fi

# THE CONTROLS: each rule must be able to FAIL, with its exact message, or the
# green line above is the silence of a detector that cannot see.
#
# A CONTROL THAT DOES NOT RUN MUST FAIL THE TEST. A label double-quoted around
# `${{ ... }}` is a bash "bad substitution": the call never happens, no PASS
# and no FAIL is counted, and the run is green one control short. So every
# wf_* call counts itself into WF_RAN, and the end of the file holds that to
# the number of call sites -- counted from this file, not typed by hand.
WF_RAN=0
# wf_control <label> <exact finding> -- the fixture is on stdin.
wf_control() {
    local f="$TMP/wf-control.yml" out rc
    cat > "$f"
    WF_RAN=$((WF_RAN + 1))
    out=$("$CHECK_WF" "$f" 2>&1); rc=$?
    if [[ "$rc" -eq 1 && "$out" == "$2" ]]; then
        pass "control: $1"
    else
        fail "control: $1 -- exit $rc, output: $out"
    fi
}
wf_control "a suite job that keeps no per-test logs on failure fails" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: always()
        uses: actions/upload-artifact@v7
        with:
          path: build/x64-debug/ctest-junit.xml
EOF
wf_control "an upload that runs only on success fails" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: success()
        uses: actions/upload-artifact@v7
        with:
          path: build/x64-debug/integration-logs
EOF
wf_control "an upload with no if (success only) fails" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        uses: actions/upload-artifact@v7
        with:
          path: build/x64-debug/integration-logs
EOF
wf_control "a negated failure() fails" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: ${{ !failure() }}
        uses: actions/upload-artifact@v7
        with:
          path: build/x64-debug/integration-logs
EOF
wf_control "an upload BEFORE the suite step fails" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - name: logs
        if: failure() || cancelled()
        uses: actions/upload-artifact@v7
        with:
          path: build/x64-debug/integration-logs
      - run: ./scripts/release-suite.sh --ci
EOF
wf_control "a step that is not upload-artifact fails" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: failure() || cancelled()
        uses: actions/cache@v4
        with:
          path: build/x64-debug/integration-logs
EOF
wf_control "a suite run AGAIN after the upload fails (the upload covers only the first)" \
    "$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: failure() || cancelled()
        uses: actions/upload-artifact@v7
        with:
          path: build/x64-debug/integration-logs
      - run: ./scripts/release-suite.sh --ci
EOF
# wf_pass <label> -- a fixture (stdin) the rule must ACCEPT.
wf_pass() {
    local f="$TMP/wf-ok.yml" out rc
    cat > "$f"
    out=$("$CHECK_WF" "$f" 2>&1); rc=$?
    WF_RAN=$((WF_RAN + 1))
    if [[ "$rc" -eq 0 && "$out" =~ \ 1\ suite\ jobs\ keep\ logs,\ 0\ container\ jobs\ trust\ the\ checkout$ ]]; then
        pass "control: $1 passes"
    else fail "control: $1 -- exit $rc, output: $out"; fi
}
wf_pass "failure() || cancelled(), a block path list" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: failure() || cancelled()
        uses: actions/upload-artifact@v7
        with:
          path: |
            build/x64-debug/integration-logs
            build/x64-debug/Testing/Temporary
EOF
wf_pass '${{ always() }}, a scalar path with a trailing slash' <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: ${{ always() }}
        uses: actions/upload-artifact@v7
        with:
          path: ./build/x64-debug/integration-logs/
EOF
wf_pass "!success(), the logs as ./…/**" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --ci
      - name: logs
        if: '!success()'
        uses: actions/upload-artifact@v7
        with:
          path: ./build/x64-debug/integration-logs/**
EOF
# A job that only MENTIONS the suite, or runs one step of it, is not a suite job.
wf_pass_none() {
    local f="$TMP/wf-ok.yml" out rc
    cat > "$f"
    out=$("$CHECK_WF" "$f" 2>&1); rc=$?
    WF_RAN=$((WF_RAN + 1))
    if [[ "$rc" -eq 0 && "$out" =~ \ 0\ suite\ jobs\ keep\ logs,\ 0\ container\ jobs\ trust\ the\ checkout$ ]]; then
        pass "control: $1 is not a suite job, nor a container job over a checkout"
    else fail "control: $1 -- exit $rc, output: $out"; fi
}
wf_pass_none "release-suite.sh --step build" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: ./scripts/release-suite.sh --step build
EOF
wf_pass_none "a comment naming release-suite.sh" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - run: |
          # later: ./scripts/release-suite.sh --ci
          true
EOF
# suite_job RUN-LINE [PATH] -- a job that runs RUN-LINE, then uploads PATH
# (default: nothing) on failure.
suite_job() {
    printf '%s\n' 'jobs:' '  j:' '    runs-on: x' '    timeout-minutes: 5' '    steps:' \
        '      - run: |' "          $1"
    [[ -n "${2:-}" ]] && printf '%s\n' '      - name: logs' '        if: failure() || cancelled()' \
        '        uses: actions/upload-artifact@v7' '        with:' "          path: $2"
    return 0
}
NOLOGS="$TMP/wf-control.yml: job j: runs release-suite.sh but uploads no build/x64-debug/integration-logs after it on failure (if: failure() || cancelled(), or always())"
# --step integration IS the step that writes the logs: only another step is exempt.
wf_control "release-suite.sh --step integration is a suite run" "$NOLOGS" \
    < <(suite_job './scripts/release-suite.sh --step integration')
# The suite however it is spelled: through bash -c, from an absolute or
# variable root, or from inside scripts/.
wf_control "bash -c \"./scripts/release-suite.sh --ci\" is a suite run" "$NOLOGS" \
    < <(suite_job "bash -c \"./scripts/release-suite.sh --ci\"")
wf_control "\"\$GITHUB_WORKSPACE/scripts/release-suite.sh\" is a suite run" "$NOLOGS" \
    < <(suite_job '"$GITHUB_WORKSPACE/scripts/release-suite.sh" --ci')
wf_control "\$ROOT/scripts/release-suite.sh is a suite run" "$NOLOGS" \
    < <(suite_job '$ROOT/scripts/release-suite.sh --ci')
wf_control "cd scripts && ./release-suite.sh is a suite run" "$NOLOGS" \
    < <(suite_job 'cd scripts && ./release-suite.sh --ci')
wf_control "a suite run redirected (>log) is a suite run" "$NOLOGS" \
    < <(suite_job './scripts/release-suite.sh>suite.log')
wf_control "a suite run in backticks is a suite run" "$NOLOGS" \
    < <(suite_job 'echo `./scripts/release-suite.sh`')
# Another command's --step on the same line does not exempt the suite.
wf_control "another command's --step on the line does not exempt the suite" "$NOLOGS" \
    < <(suite_job './scripts/release-suite.sh --ci && ./scripts/other.sh --step build')
# ...and a file that merely ends in the name is not it.
wf_pass_none "myrelease-suite.sh is not the suite" < <(suite_job './scripts/myrelease-suite.sh --ci')
# A parent above build/x64-debug uploads the whole build tree (hundreds of MB).
# Any parent of the logs -- build/x64-debug included -- uploads a whole build
# tree (hundreds of MB) for ~2 MB of logs.
PARENTMSG="a parent of build/x64-debug/integration-logs -- a whole build tree (hundreds of MB), not the logs; name build/x64-debug/integration-logs"
wf_control "uploading build/ fails, named" \
    "$TMP/wf-control.yml: job j step 'logs': uploads 'build/', $PARENTMSG
$NOLOGS" < <(suite_job './scripts/release-suite.sh --ci' 'build/')
wf_control "uploading build/** fails, named" \
    "$TMP/wf-control.yml: job j step 'logs': uploads 'build/**', $PARENTMSG
$NOLOGS" < <(suite_job './scripts/release-suite.sh --ci' 'build/**')
wf_control "uploading build/x64-debug fails, named" \
    "$TMP/wf-control.yml: job j step 'logs': uploads 'build/x64-debug', $PARENTMSG
$NOLOGS" < <(suite_job './scripts/release-suite.sh --ci' 'build/x64-debug')

# RULE 5: a job in a container trusts the checkout before it runs anything over it.
TRUSTMSG="runs after actions/checkout, in a container, before git trusts the checkout (git config --global --add safe.directory \"\$GITHUB_WORKSPACE\")"
wf_control "a container job that runs a step before trusting the checkout fails" \
    "$TMP/wf-control.yml: job j step 'build': $TRUSTMSG" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - name: build
        run: ./scripts/make-release-assets.sh host-tools
      - run: git config --global --add safe.directory "$GITHUB_WORKSPACE"
EOF
wf_control "a trust line that is not the step's first command does not count" \
    "$TMP/wf-control.yml: job j step 'build': $TRUSTMSG" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container:
      image: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - name: build
        run: |
          ./scripts/axl-build.sh ARCH=x64 all
          git config --global --add safe.directory "$GITHUB_WORKSPACE"
EOF
wf_control "trusting some other directory does not count (the step itself is named)" \
    "$TMP/wf-control.yml: job j step '#2': $TRUSTMSG" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - run: git config --global --add safe.directory /tmp
      - name: build
        run: ./scripts/axl-build.sh ARCH=x64 all
EOF
# Single quotes keep the shell from expanding the variable: git would trust
# the literal text `$GITHUB_WORKSPACE`.
wf_control "a single-quoted \$GITHUB_WORKSPACE does not count" \
    "$TMP/wf-control.yml: job j step '#2': $TRUSTMSG" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - run: git config --global --add safe.directory '$GITHUB_WORKSPACE'
      - run: ./scripts/axl-build.sh ARCH=x64 all
EOF
# A trust step that may not run trusts nothing.
wf_control "a trust step with an if: does not count" \
    "$TMP/wf-control.yml: job j step 'trust': $TRUSTMSG" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - name: trust
        if: matrix.arch == 'x64'
        run: git config --global --add safe.directory "$GITHUB_WORKSPACE"
      - run: ./scripts/axl-build.sh ARCH=x64 all
EOF
# A checkout elsewhere needs THAT directory trusted, which the rule cannot see.
wf_control "a container job's checkout into a path: is refused, named" \
    "$TMP/wf-control.yml: job j: checks out into path 'src' in a container; rule 5 trusts only the workspace -- check out into the workspace" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
        with:
          path: src
      - run: git config --global --add safe.directory "$GITHUB_WORKSPACE"
EOF
# An install step that also runs the tree is not an install step.
wf_control "an apt step that also runs a script from the checkout is not exempt" \
    "$TMP/wf-control.yml: job j step 'deps': $TRUSTMSG" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:24.04
    steps:
      - uses: actions/checkout@v5
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() {
            timeout -k 10 180 "$@"
          }
          apt_wall apt-get install -y git
          ./scripts/make-release-assets.sh host-tools
      - run: git config --global --add safe.directory "$GITHUB_WORKSPACE"
EOF
# install_job LINE -- a container job whose post-checkout package step also
# holds LINE; that line runs code from (or near) the checkout, so the step is
# not a package-install step and must be flagged.
install_job() {
    printf '%s\n' 'jobs:' '  j:' '    timeout-minutes: 5' '    container: ubuntu:24.04' '    steps:' \
        '      - uses: actions/checkout@v5' '      - name: deps' '        run: |' \
        "          printf 'x\\n' > /etc/apt/apt.conf.d/99-axl-retry" \
        '          apt_wall() {' '            timeout -k 10 180 "$@"' '          }' \
        '          apt_wall apt-get install -y git' "          $1" \
        '      - run: git config --global --add safe.directory "$GITHUB_WORKSPACE"'
}
INSTALLMSG="$TMP/wf-control.yml: job j step 'deps': $TRUSTMSG"
wf_control "apt_wall running a script is not package installation" "$INSTALLMSG" \
    < <(install_job 'apt_wall ./scripts/build-docs.sh')
wf_control "apt_wall of plain words that are not apt is not package installation" "$INSTALLMSG" \
    < <(install_job 'apt_wall bash build-docs.sh')
wf_control "apt_wall with a process substitution is not package installation" "$INSTALLMSG" \
    < <(install_job 'apt_wall apt-get install -y <(./scripts/x.sh)')
wf_control "a pip install from a requirements file is not package installation" "$INSTALLMSG" \
    < <(install_job 'pip3 install -r docs/requirements.txt')
wf_control "a pip install of the checkout is not package installation" "$INSTALLMSG" \
    < <(install_job 'pip install -e .')
wf_control "a printf that runs a command before its redirect is not package installation" "$INSTALLMSG" \
    < <(install_job 'printf x; ./scripts/build-docs.sh > /etc/apt/apt.conf.d/99-axl-retry')
wf_control "an export of anything but DEBIAN_FRONTEND is not package installation" "$INSTALLMSG" \
    < <(install_job 'export BASH_ENV=./scripts/evil.sh')
# wf_trusts <label> -- a container job (stdin) rule 5 must ACCEPT, and count.
wf_trusts() {
    local f="$TMP/wf-ok.yml" out rc
    cat > "$f"
    WF_RAN=$((WF_RAN + 1))
    out=$("$CHECK_WF" "$f" 2>&1); rc=$?
    if [[ "$rc" -eq 0 && "$out" =~ \ 1\ container\ jobs\ trust\ the\ checkout$ ]]; then pass "control: $1 passes"
    else fail "control: $1 -- exit $rc, output: $out"; fi
}
wf_trusts "a trust step between the checkout and the first run" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - name: Trust the mounted workspace
        run: git config --global --add safe.directory "$GITHUB_WORKSPACE"
      - uses: actions/cache@v4
      - run: ./scripts/axl-build.sh ARCH=x64 all
EOF
wf_trusts "trust before the checkout, as the first command of a step, \${{ github.workspace }}" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - run: |
          echo deps
          git config --global --add safe.directory '${{ github.workspace }}'
      - uses: actions/checkout@v5
      - run: ./scripts/axl-build.sh ARCH=x64 all
EOF
wf_trusts "the trust as the first command of the first step after the checkout" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
      - run: |
          git config --global --add safe.directory "$GITHUB_WORKSPACE"
          ./scripts/axl-build.sh ARCH=x64 all
EOF
# docs.yml's shape: the checkout comes before git is installed (so it is a
# REST tarball), and the package step that installs git is the one step that
# may run before the trust -- it cannot trust with a git it has not installed.
wf_trusts "a package-install step between the checkout and the trust" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:24.04
    steps:
      - uses: actions/checkout@v5
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() {
            timeout -k 10 180 "$@"
          }
          export DEBIAN_FRONTEND=noninteractive
          apt_wall apt-get -q --error-on=any update
          apt_wall apt-get install -y -qq doxygen python3-pip git
          pip3 install --no-cache-dir --break-system-packages sphinx==9.1.0
      - run: git config --global --add safe.directory "$GITHUB_WORKSPACE"
      - run: ./scripts/build-docs.sh
EOF
# A job with no container, or a container job that checks nothing out, is not
# a container job over a checkout: not flagged, and not counted.
wf_pass_none "a container job with no checkout" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - run: echo hi
EOF
wf_pass_none "a container job that runs nothing after its checkout" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    container: ubuntu:26.04
    steps:
      - uses: actions/checkout@v5
EOF
wf_control "a job with no timeout-minutes fails" \
    "$TMP/wf-control.yml: job j: has no timeout-minutes (GitHub's default is 6 hours)" <<'EOF'
jobs:
  j:
    runs-on: x
    steps:
      - run: true
EOF
wf_control "a non-integer timeout-minutes fails" \
    "$TMP/wf-control.yml: job j: timeout-minutes is '10m', not a positive integer" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 10m
    steps:
      - run: true
EOF
wf_control "a duplicate key fails" \
    "$TMP/wf-control.yml: duplicate key 'needs'" <<'EOF'
jobs:
  j:
    needs: a
    needs: b
    timeout-minutes: 5
EOF
wf_control "a bare apt-get fails, named" \
    "$TMP/wf-control.yml: job j step 'deps': calls apt-get without defining apt_wall first
$TMP/wf-control.yml: job j step 'deps': calls apt-get without writing 99-axl-retry first
$TMP/wf-control.yml: job j step 'deps': apt-get not wrapped in apt_wall: apt-get install -y -qq gcc" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          apt-get install -y -qq gcc
EOF
wf_control "an update without --error-on=any fails" \
    "$TMP/wf-control.yml: job j step 'deps': apt-get update without --error-on=any: apt_wall sudo apt-get -q update" <<'EOF'
jobs:
  j:
    runs-on: x
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          printf 'x\n' | sudo tee /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() { timeout -k 10 180 "$@"; }
          apt_wall sudo apt-get -q \
            update
EOF
wf_control "an install behind a probe of apt-get is still seen" \
    "$TMP/wf-control.yml: job j step 'deps': apt-get not wrapped in apt_wall: command -v apt-get >/dev/null && apt-get install -y gcc" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() {
            timeout -k 10 180 "$@"
          }
          command -v apt-get >/dev/null && apt-get install -y gcc
EOF
wf_control "timeout-minutes: 0 fails" \
    "$TMP/wf-control.yml: job j: timeout-minutes is 0, not a positive integer" <<'EOF'
jobs:
  j:
    timeout-minutes: 0
EOF
wf_control "apt_wall defined only AFTER the first call fails" \
    "$TMP/wf-control.yml: job j step 'deps': calls apt-get without defining apt_wall first" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall apt-get install -y gcc
          apt_wall() {
            timeout -k 10 180 "$@"
          }
EOF
wf_control "an apt_wall that bounds nothing fails" \
    "$TMP/wf-control.yml: job j step 'deps': apt_wall does not run its command under \`timeout -k\`" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() {
            "$@"
          }
          apt_wall apt install -y gcc
EOF
wf_control "two apt_wall copies that differ fail" \
    "$TMP/wf-control.yml: job k step 'deps': apt_wall differs from the copy in $TMP/wf-control.yml: job j step 'deps'" <<'EOF'
jobs:
  j:
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() {
            timeout -k 10 180 "$@"
          }
          apt_wall apt-get -q --error-on=any update
  k:
    timeout-minutes: 5
    steps:
      - name: deps
        run: |
          printf 'x\n' > /etc/apt/apt.conf.d/99-axl-retry
          apt_wall() {
            timeout -k 10 999 "$@"
          }
          apt_wall apt-get install -y update-notifier
EOF

# Every wf_* call site above ran (see WF_RAN). A call is a line that STARTS
# with the function's name and a space; the definitions (`wf_x() {`) do not.
_wf_sites=$(grep -cE '^[[:space:]]*wf_(control|pass|pass_none|trusts) ' "${BASH_SOURCE[0]}")
if [[ "$_wf_sites" -gt 0 && "$WF_RAN" -eq "$_wf_sites" ]]; then
    pass "all $_wf_sites workflow controls ran"
else
    fail "workflow controls: $WF_RAN ran of $_wf_sites call sites -- one never ran (a quoting error in its call?)"
fi

echo ""
echo "ci-plan: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
